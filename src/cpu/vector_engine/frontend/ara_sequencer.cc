/* SPDX-License-Identifier: BSD-3-Clause */

#include "cpu/vector_engine/frontend/ara_sequencer.hh"

#include <limits>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/vector_engine/common/vector_support.hh"
#include "debug/VectorTimeline.hh"

namespace gem5::vector_engine
{

AraSequencer::AraSequencer(ClockedObject &owner, CommandQueue &queue,
                           CpuCompletionEndpoint &completion_endpoint,
                           uint32_t vlen_bytes, uint32_t max_active_commands,
                           ArithmeticSender send_arithmetic,
                           MemorySender send_memory, SlideSender send_slide)
    : stats(&owner, "sequencer", "commands"),
      arithmeticCommands(&stats, "arithmeticCommands",
                         statistics::units::Count::get(),
                         "Arithmetic commands started"),
      loadCommands(&stats, "loadCommands", statistics::units::Count::get(),
                   "Vector load commands started"),
      storeCommands(&stats, "storeCommands", statistics::units::Count::get(),
                    "Vector store commands started"),
      slideCommands(&stats, "slideCommands", statistics::units::Count::get(),
                    "Vector slide commands started"),
      emptyCommands(&stats, "emptyCommands", statistics::units::Count::get(),
                    "Commands completed without creating a backend task"),
      overlappedStarts(&stats, "overlappedStarts",
                       statistics::units::Count::get(),
                       "Commands started while another command was active"),
      multiActiveTicks(&stats, "multiActiveTicks",
                       statistics::units::Tick::get(),
                       "Ticks with two or more active commands; open "
                       "intervals are split at each dump"),
      capacityWaitTicks(&stats, "capacityWaitTicks",
                        statistics::units::Tick::get(),
                        "Ticks the queue head waited for an active slot"),
      memoryBarrierWaitTicks(&stats, "memoryBarrierWaitTicks",
                             statistics::units::Tick::get(),
                             "Ticks the queue head waited at a memory "
                             "barrier"),
      isolationWaitTicks(&stats, "isolationWaitTicks",
                         statistics::units::Tick::get(),
                         "Ticks the queue head waited for nonzero-vstart "
                         "isolation"),
      hazardWaitTicks(&stats, "hazardWaitTicks",
                      statistics::units::Tick::get(),
                      "Ticks the queue head waited on any RAW/WAR/WAW "
                      "hazard; each interval counted once"),
      rawWaitTicks(&stats, "rawWaitTicks", statistics::units::Tick::get(),
                   "Ticks the queue head waited on a RAW hazard; overlaps "
                   "WAR/WAW, do not add"),
      warWaitTicks(&stats, "warWaitTicks", statistics::units::Tick::get(),
                   "Ticks the queue head waited on a WAR hazard; overlaps "
                   "RAW/WAW, do not add"),
      wawWaitTicks(&stats, "wawWaitTicks", statistics::units::Tick::get(),
                   "Ticks the queue head waited on a WAW hazard; overlaps "
                   "RAW/WAR, do not add"),
      owner(owner),
      queue(queue),
      completionEndpoint(completion_endpoint),
      vlenBytes(vlen_bytes),
      maxActiveCommands(max_active_commands),
      sendArithmetic(std::move(send_arithmetic)),
      sendMemory(std::move(send_memory)),
      sendSlide(std::move(send_slide)),
      progressEvent([this] { evaluate(); }, owner.name() + ".ara_sequencer")
{
    fatal_if(vlenBytes == 0, "AraSequencer requires nonzero VLEN");
    fatal_if(maxActiveCommands == 0,
             "AraSequencer requires at least one active command");
    fatal_if(!sendArithmetic || !sendMemory || !sendSlide,
             "AraSequencer requires all task senders");
    active.reserve(maxActiveCommands);
    // Las esperas de la cabeza y la ocupación son intervalos propios del
    // secuenciador; sin estos ganchos, un volcado perdería el tramo abierto
    // y un reset dejaría entrar tiempo previo en la región medida.
    stats.setIntervalHooks([this] { flushOpenIntervals(); },
                           [this] { restartOpenIntervals(); });
}

AraSequencer::~AraSequencer()
{
    if (progressEvent.scheduled()) {
        owner.deschedule(progressEvent);
    }
}

void
AraSequencer::wakeup()
{
    if (progressEvent.scheduled()) {
        return;
    }

    // Hay algo que examinar si queda una cabeza por iniciar, una tarea por
    // enviar o una finalización por entregar. Las tareas aceptadas sin
    // respuesta progresan en su unidad y no requieren evaluación propia.
    bool needs_progress = !queue.empty();
    for (const auto &state : active) {
        if (state.completion || (state.task && !state.taskAccepted)) {
            needs_progress = true;
        }
    }

    if (needs_progress) {
        owner.schedule(progressEvent, owner.clockEdge(Cycles(1)));
    }
}

const UnitTask &
AraSequencer::taskOf(const detail::ActiveCommandState &state)
{
    panic_if(!state.task, "Active command has no backend task");

    return std::visit(
        [](const auto &task) -> const UnitTask & { return task.task; },
        *state.task);
}

bool
AraSequencer::isMemory(const VectorCommand &command)
{
    return std::holds_alternative<MemoryCommand>(command.payload);
}

AraSequencer::BackendTask
AraSequencer::makeTask(const VectorCommand &command) const
{
    const auto &config = command.config;

    // Precondición: comando validado estructuralmente y admitido.
    panic_if(!command.command.valid() ||
                 !VectorSupport::supportsConfiguration(config),
             "Invalid admitted configuration");

    panic_if(config.vstart >= config.vl,
             "Cannot create a task for an empty range");

    panic_if(config.vl > std::numeric_limits<uint32_t>::max() ||
                 config.vstart > std::numeric_limits<uint32_t>::max(),
             "Task element range is not representable");
    const ElementRange elements{
        static_cast<uint32_t>(config.vstart),
        static_cast<uint32_t>(config.vl - config.vstart)};

    // Calcular antes en 64 bits: ByteRange usa campos de 32 bits.
    const auto element_bytes = VectorSupport::elementBytes(config.sewBits);
    const uint64_t offset = uint64_t{elements.firstElement} * element_bytes;
    const uint64_t size = uint64_t{elements.elementCount} * element_bytes;
    const uint64_t end = offset + size;

    panic_if(end > std::numeric_limits<uint32_t>::max(),
             "Task byte range is not representable");

    const auto effective_bytes =
        VectorSupport::groupBytes(vlenBytes, config.lmul);
    panic_if(end > effective_bytes, "Task exceeds effective LMUL capacity");

    const ByteRange range{static_cast<uint32_t>(offset),
                          static_cast<uint32_t>(size)};

    const auto expected_regs = VectorSupport::groupRegisterCount(config.lmul);

    const auto check_register = [&](const VectorRegRef &reg) {
        panic_if(!reg.naturallyAligned() || reg.regCount != expected_regs ||
                     end > uint64_t{vlenBytes} * reg.regCount,
                 "Task range does not fit its register group");
    };

    // Una sola tarea por comando: cero es único dentro de CommandKey.
    const TaskKey key{command.command, TaskId{0}};

    // Comprobar todos los registros que lee o escribe la operación. El
    // escalar y la dirección ya capturados no son grupos del VRF.
    for (const auto &operand : describeOperands(command)) {
        if (const auto *reg = std::get_if<VectorRegRef>(&operand.value)) {
            check_register(*reg);
        }
    }

    if (const auto *arithmetic =
            std::get_if<ArithmeticCommand>(&command.payload)) {
        return ArithmeticTask{UnitTask{key, VectorUnitClass::Lanes, elements},
                              config, *arithmetic, range, command.fpContext};
    }

    if (const auto *slide = std::get_if<SlideCommand>(&command.payload)) {
        // El rango es el de destino; SlideUnit deriva el de fuente.
        return SlideTask{UnitTask{key, VectorUnitClass::Slide, elements},
                         config, *slide, range};
    }

    const auto &memory = std::get<MemoryCommand>(command.payload);

    return MemoryTask{UnitTask{key, VectorUnitClass::Vlsu, elements},
                      config,
                      memory,
                      range,
                      command.pc,
                      command.requestorId};
}

AraSequencer::StartBlockers
AraSequencer::blockersFor(const VectorCommand &command) const
{
    StartBlockers blockers;

    blockers.capacity = active.size() >= maxActiveCommands;

    // La memoria actúa como barrera (diseño, sección 7.1): una carga o un
    // store no se solapa con aritmética ni con otra instrucción de memoria.
    // Un comando con vstart != 0 reanuda una instrucción interrumpida y se
    // ejecuta aislado (sección 7). Ambas reglas se aplican en los dos
    // sentidos: al comando que quiere iniciar y a los ya activos.
    for (const auto &state : active) {
        if (isMemory(state.command)) {
            blockers.memoryBarrier = true;
        }
        if (state.command.config.vstart != 0) {
            blockers.isolation = true;
        }
    }
    if (!active.empty()) {
        if (isMemory(command)) {
            blockers.memoryBarrier = true;
        }
        if (command.config.vstart != 0) {
            blockers.isolation = true;
        }
    }

    blockers.hazards =
        scoreboard.hazards(VectorScoreboard::footprint(command));

    return blockers;
}

void
AraSequencer::accountHeadWait(const std::optional<VectorCommand> &head,
                              const StartBlockers &blockers)
{
    // Cerrar el intervalo anterior con las causas que estaban vigentes.
    if (headWait) {
        closeHeadWaitPart();
        headWait.reset();
    }

    // Abrir uno nuevo si la cabeza sigue bloqueada.
    if (head && blockers.any()) {
        headWait = HeadWait{head->command, curTick(), blockers};
    }
}

void
AraSequencer::closeHeadWaitPart()
{
    // Suma a cada causa vigente el tramo [since, curTick()) y lo reinicia
    // en curTick(). La espera puede seguir abierta: accountHeadWait la
    // cierra al cambiar de causas y flushOpenIntervals al volcar.
    // Las causas no son excluyentes; sumar contadores distintos no da el
    // tiempo total de espera.
    const Tick elapsed = curTick() - headWait->since;
    const auto &current = headWait->blockers;
    if (current.capacity) {
        capacityWaitTicks += elapsed;
    }
    if (current.memoryBarrier) {
        memoryBarrierWaitTicks += elapsed;
    }
    if (current.isolation) {
        isolationWaitTicks += elapsed;
    }
    // hazardWaitTicks cuenta el intervalo una vez aunque haya varios
    // tipos de dependencia; los tres siguientes lo repiten por tipo.
    if (current.hazards.any()) {
        hazardWaitTicks += elapsed;
    }
    if (current.hazards.raw) {
        rawWaitTicks += elapsed;
    }
    if (current.hazards.war) {
        warWaitTicks += elapsed;
    }
    if (current.hazards.waw) {
        wawWaitTicks += elapsed;
    }
    headWait->since = curTick();
}

void
AraSequencer::noteOccupancyChange()
{
    // Se llama antes de cambiar active.size() y antes de cada volcado de
    // estadísticas: suma el tramo con dos o más comandos activos hasta
    // curTick() y empieza el siguiente ahí.
    if (active.size() >= 2) {
        multiActiveTicks += curTick() - lastOccupancyChange;
    }
    lastOccupancyChange = curTick();
}

void
AraSequencer::flushOpenIntervals()
{
    // Antes de un volcado: cada periodo cuenta el tramo abierto que le
    // corresponde y el resto se cuenta en el siguiente.
    if (headWait) {
        closeHeadWaitPart();
    }
    noteOccupancyChange();
}

void
AraSequencer::restartOpenIntervals()
{
    // Tras un reset: el tiempo anterior pertenece al periodo descartado.
    if (headWait) {
        headWait->since = curTick();
    }
    lastOccupancyChange = curTick();
}

void
AraSequencer::start(const VectorCommand &command)
{
    if (std::holds_alternative<ArithmeticCommand>(command.payload)) {
        ++arithmeticCommands;
    } else if (std::holds_alternative<SlideCommand>(command.payload)) {
        ++slideCommands;
    } else if (std::get<MemoryCommand>(command.payload).direction ==
               MemoryDirection::Load) {
        ++loadCommands;
    } else {
        ++storeCommands;
    }
    if (!active.empty()) {
        ++overlappedStarts;
    }

    uint64_t count = 0;
    if (command.config.vl > command.config.vstart) {
        count = command.config.vl - command.config.vstart;
    }
    stats.accept(count, uint64_t{count} * VectorSupport::elementBytes(
                                              command.config.sewBits));

    // AraSequencer no es un SimObject: se usa el logger sin name().
    if (GEM5_UNLIKELY(TRACING_ON && ::gem5::debug::VectorTimeline)) {
        ::gem5::trace::getDebugLogger()->dprintf_flag(
            ::gem5::curTick(), "vectorEngine.sequencer", "VectorTimeline",
            "start cmd=%llu\n", command.command.commandId);
    }

    // La reserva se toma antes de crear la tarea: desde este punto los
    // comandos posteriores ven sus registros como ocupados.
    scoreboard.reserve(command.command, VectorScoreboard::footprint(command));
    queue.markStarted(command.command);

    noteOccupancyChange();
    active.emplace_back();
    auto &state = active.back();
    state.command = command;

    if (command.config.vstart >= command.config.vl) {
        // Rango vacío: no hay accesos ni paquetes. Se cierra en la próxima
        // evaluación con una finalización sintética, sin tarea de unidad.
        ++emptyCommands;
        state.completion.emplace();
        return;
    }

    state.task = makeTask(command);
}

bool
AraSequencer::sendPendingTasks()
{
    // Enviar en orden de inicio. Una tarea más antigua en Retry retiene a
    // las posteriores para que cada unidad las reciba en orden.
    for (std::size_t i = 0; i < active.size(); ++i) {
        if (!active[i].task || active[i].taskAccepted) {
            continue;
        }

        // Reservar recepción antes de llamar a una unidad: puede responder
        // sincrónicamente. recvCompletion guarda la respuesta, no retira.
        // Se accede por índice porque el vector no cambia durante el envío.
        active[i].taskAccepted = true;

        TransferResult result;
        if (const auto *arithmetic =
                std::get_if<ArithmeticTask>(&*active[i].task)) {
            result = sendArithmetic(*arithmetic);
        } else if (const auto *slide =
                       std::get_if<SlideTask>(&*active[i].task)) {
            result = sendSlide(*slide);
        } else {
            result = sendMemory(std::get<MemoryTask>(*active[i].task));
        }

        switch (result) {
            case TransferResult::Accepted:
                break;

            case TransferResult::Retry:
                ++stats.retries;
                panic_if(active[i].completion.has_value(),
                         "A unit completed a task that it rejected");
                active[i].taskAccepted = false;
                return false;

            default:
                panic("Invalid task transfer result");
        }
    }
    return true;
}

bool
AraSequencer::finishCompleted()
{
    bool finished = false;
    std::size_t i = 0;
    while (i < active.size()) {
        if (active[i].completion) {
            // finish() elimina la entrada; el índice ya apunta a la siguiente.
            finish(i);
            finished = true;
        } else {
            ++i;
        }
    }
    return finished;
}

void
AraSequencer::evaluate()
{
    // 1. Entregar las finalizaciones recibidas, en cualquier orden.
    const bool finished = finishCompleted();

    // 2. Iniciar la cabeza si nada lo impide. Un cierre en este ciclo deja
    //    libres sus recursos a partir del siguiente, como en la entrega
    //    serie anterior; se inicia como máximo un comando por ciclo.
    bool started = false;
    const auto head = queue.front();
    StartBlockers blockers;
    if (head && !finished) {
        blockers = blockersFor(*head);
        if (!blockers.any()) {
            start(*head);
            started = true;
        }
    }
    // Si hubo cierres, la cabeza no se evaluó y su espera sigue abierta con
    // las causas anteriores hasta la próxima evaluación.
    if (!finished) {
        accountHeadWait(head, blockers);
    }

    // 3. Enviar las tareas pendientes, también la del comando recién
    //    iniciado. Una finalización síncrona sólo se registra aquí.
    const bool all_sent = sendPendingTasks();

    // Reevaluar si hubo progreso o un Retry. Una cabeza bloqueada sin
    // cambios no se sondea: la despertará una finalización o un dispatch.
    if (finished || started || !all_sent) {
        wakeup();
    }
}

void
AraSequencer::recvCompletion(const UnitCompletion &completion)
{
    detail::ActiveCommandState *found = nullptr;
    for (auto &state : active) {
        if (state.command.command == completion.key.command) {
            found = &state;
            break;
        }
    }
    panic_if(!found || !found->task || !found->taskAccepted,
             "Completion without an issued task");

    const auto &task = taskOf(*found);

    panic_if(!validFpFlags(completion.fpFlags) ||
                 (!found->command.fpContext && completion.fpFlags),
             "Unexpected unit floating-point effects");

    panic_if(completion.key != task.key,
             "Completion does not match active TaskKey");

    panic_if(found->completion.has_value(), "Duplicate task completion");

    switch (completion.status) {
        case UnitCompletionStatus::Success:
            panic_if(completion.fault.has_value(),
                     "Successful completion contains a fault");
            break;

        case UnitCompletionStatus::MemoryFault: {
            panic_if(task.unit != VectorUnitClass::Vlsu,
                     "Arithmetic task reported a memory fault");

            panic_if(!completion.fault, "Memory fault lacks FaultInfo");

            const auto &fault = *completion.fault;

            panic_if(fault.fault == NoFault || !fault.address.has_value() ||
                         !fault.elementIndex.has_value(),
                     "Incomplete memory fault information");

            const auto index = *fault.elementIndex;

            panic_if(index < task.elements.firstElement ||
                         index >= task.elements.end(),
                     "Fault element is outside the active task");
            break;
        }

        default:
            panic("Invalid unit completion status");
    }

    found->completion = completion;
    wakeup();
}

void
AraSequencer::finish(std::size_t index)
{
    panic_if(index >= active.size() || !active[index].completion,
             "Cannot finish a command without completion");

    const auto &state = active[index];
    if (state.task) {
        panic_if(!state.taskAccepted, "Cannot finish before task completion");
    } else {
        panic_if(state.command.config.vstart < state.command.config.vl,
                 "Nonempty command has no task");
    }

    VectorCompletion completion;
    completion.command = state.command.command;
    completion.status = CompletionStatus::Success;
    completion.finalVstart = 0;
    completion.fpFlags = state.completion->fpFlags;

    if (state.completion->status == UnitCompletionStatus::MemoryFault) {
        completion.status = CompletionStatus::MemoryFault;
        completion.fault = state.completion->fault;
        completion.finalVstart = *completion.fault->elementIndex;
    }

    // La unidad ya confirmó todos sus writebacks: los registros quedan
    // libres para los comandos posteriores (sin chaining).
    scoreboard.release(completion.command);

    // Notificar antes de liberar el crédito: durante la llamada la CPU debe
    // seguir viendo este comando como pendiente. Después se permite avanzar.
    // El callback no modifica active, de modo que index sigue siendo válido.
    completionEndpoint.completed(completion);
    stats.complete(completion.status != CompletionStatus::Success);
    queue.release(completion.command);

    noteOccupancyChange();
    active.erase(active.begin() + index);
}

} // namespace gem5::vector_engine
