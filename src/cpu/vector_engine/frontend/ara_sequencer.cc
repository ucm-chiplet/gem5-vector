/* SPDX-License-Identifier: BSD-3-Clause */

#include "cpu/vector_engine/frontend/ara_sequencer.hh"

#include <limits>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

AraSequencer::AraSequencer(ClockedObject &owner, CommandQueue &queue,
                           CpuCompletionEndpoint &completion_endpoint,
                           uint32_t vlen_bytes,
                           ArithmeticSender send_arithmetic,
                           MemorySender send_memory)
    : owner(owner),
      queue(queue),
      completionEndpoint(completion_endpoint),
      vlenBytes(vlen_bytes),
      sendArithmetic(std::move(send_arithmetic)),
      sendMemory(std::move(send_memory)),
      progressEvent([this] { evaluate(); }, owner.name() + ".ara_sequencer")
{
    fatal_if(vlenBytes == 0, "AraSequencer requires nonzero VLEN");
    fatal_if(!sendArithmetic || !sendMemory,
             "AraSequencer requires both task senders");
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

    if (!active && queue.empty()) {
        return;
    }

    // Una tarea aceptada progresa en su unidad hasta recibir respuesta.
    if (active && active->taskAccepted && !active->completion) {
        return;
    }

    owner.schedule(progressEvent, owner.clockEdge(Cycles(1)));
}

const UnitTask &
AraSequencer::currentTask() const
{
    panic_if(!active || !active->task, "AraSequencer has no current task");

    return std::visit(
        [](const auto &task) -> const UnitTask & { return task.task; },
        *active->task);
}

AraSequencer::BackendTask
AraSequencer::makeTask(const VectorCommand &command) const
{
    const auto &config = command.config;

    // Precondición: comando validado estructuralmente y admitido.
    panic_if(!command.command.valid() || !isValid(config.lmul) ||
                 config.sewBits != 32 || config.masked,
             "Invalid admitted configuration");

    panic_if(config.vstart >= config.vl,
             "Cannot create a task for an empty range");

    const ElementRange elements{config.vstart, config.vl - config.vstart};

    // Calcular antes en 64 bits: ByteRange usa campos de 32 bits.
    const uint64_t offset = uint64_t{elements.firstElement} * 4;
    const uint64_t size = uint64_t{elements.elementCount} * 4;
    const uint64_t end = offset + size;

    panic_if(end > std::numeric_limits<uint32_t>::max(),
             "Task byte range is not representable");

    const int exponent = static_cast<int>(config.lmul);
    const uint64_t effective_bytes = exponent >= 0
                                         ? uint64_t{vlenBytes} << exponent
                                         : uint64_t{vlenBytes} >> -exponent;

    panic_if(end > effective_bytes, "Task exceeds effective LMUL capacity");

    const ByteRange range{static_cast<uint32_t>(offset),
                          static_cast<uint32_t>(size)};

    const unsigned expected_regs = exponent > 0 ? 1U << exponent : 1U;

    const auto check_register = [&](const VectorRegRef &reg) {
        panic_if(!reg.naturallyAligned() || reg.regCount != expected_regs ||
                     end > uint64_t{vlenBytes} * reg.regCount,
                 "Task range does not fit its register group");
    };

    // Una sola tarea por comando: cero es único dentro de CommandKey.
    const TaskKey key{command.command, TaskId{0}};

    if (const auto *arithmetic =
            std::get_if<ArithmeticCommand>(&command.payload)) {
        check_register(arithmetic->destination);
        check_register(arithmetic->vectorSource);

        if (const auto *source =
                std::get_if<VectorRegRef>(&arithmetic->secondOperand)) {
            check_register(*source);
        }

        return ArithmeticTask{UnitTask{key, VectorUnitClass::Lanes, elements},
                              config, *arithmetic, range};
    }

    const auto &memory = std::get<MemoryCommand>(command.payload);
    check_register(memory.dataReg);

    return MemoryTask{UnitTask{key, VectorUnitClass::Vlsu, elements},
                      config,
                      memory,
                      range,
                      command.pc,
                      command.requestorId};
}

void
AraSequencer::evaluate()
{
    if (!active) {
        const auto command = queue.front();
        if (!command) {
            return;
        }

        active.emplace();
        active->command = *command;

        if (command->config.vstart >= command->config.vl) {
            finish();
            return;
        }

        active->task = makeTask(*command);
    }

    if (active->completion) {
        finish();
        return;
    }

    if (active->taskAccepted) {
        return;
    }

    // Reservar recepción antes de llamar a una unidad: puede responder
    // sincrónicamente. recvCompletion guarda la respuesta, no retira.
    active->taskAccepted = true;

    TransferResult result;

    if (const auto *arithmetic = std::get_if<ArithmeticTask>(&*active->task)) {
        result = sendArithmetic(*arithmetic);
    } else {
        result = sendMemory(std::get<MemoryTask>(*active->task));
    }

    switch (result) {
        case TransferResult::Accepted:
            // Si hubo respuesta inmediata, ya programó la evaluación.
            return;

        case TransferResult::Retry:
            panic_if(active->completion.has_value(),
                     "A unit completed a task that it rejected");

            active->taskAccepted = false;
            wakeup();
            return;
    }

    panic("Invalid task transfer result");
}

void
AraSequencer::recvCompletion(const UnitCompletion &completion)
{
    panic_if(!active || !active->task || !active->taskAccepted,
             "Completion without an issued task");

    const auto &task = currentTask();

    panic_if(completion.key != task.key,
             "Completion does not match active TaskKey");

    panic_if(active->completion.has_value(), "Duplicate task completion");

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

    active->completion = completion;
    wakeup();
}

void
AraSequencer::finish()
{
    panic_if(!active, "Cannot finish without an active command");

    if (active->task) {
        panic_if(!active->taskAccepted || !active->completion,
                 "Cannot finish before task completion");
    } else {
        panic_if(active->command.config.vstart < active->command.config.vl,
                 "Nonempty command has no task");
    }

    VectorCompletion completion;
    completion.command = active->command.command;
    completion.status = CompletionStatus::Success;
    completion.finalVstart = 0;

    if (active->completion &&
        active->completion->status == UnitCompletionStatus::MemoryFault) {
        completion.status = CompletionStatus::MemoryFault;
        completion.fault = active->completion->fault;
        completion.finalVstart = *completion.fault->elementIndex;
    }

    // Preparar todo el estado antes del callback externo.
    queue.releaseHead(completion.command);
    active.reset();

    // La siguiente instrucción no se ejecuta en esta llamada.
    wakeup();

    completionEndpoint.completed(completion);
}

} // namespace gem5::vector_engine
