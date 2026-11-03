// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/vlsu/ara_vlsu.hh"

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>

#include "base/logging.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

AraVLSU::AraVLSU(ClockedObject &owner, const AddressMapper &mapper,
                 const VlsuParameters &parameters, unsigned address_bits,
                 MemorySender send_memory, BoundaryQuery query_boundaries,
                 VrfWakeup wake_vrf, CompletionSender send_completion)
    : stats(&owner, "vlsu", "memory tasks"),
      loadTasks(&stats, "loadTasks", statistics::units::Count::get(),
                "Accepted vector load tasks"),
      storeTasks(&stats, "storeTasks", statistics::units::Count::get(),
                 "Accepted vector store tasks"),
      loadElements(&stats, "loadElements", statistics::units::Count::get(),
                   "Loaded elements after VRF write acknowledgement"),
      storeElements(&stats, "storeElements", statistics::units::Count::get(),
                    "Stored elements after memory acknowledgement"),
      loadBytes(&stats, "loadBytes", statistics::units::Byte::get(),
                "Bytes loaded and written back to VRF"),
      storeBytes(&stats, "storeBytes", statistics::units::Byte::get(),
                 "Bytes stored and acknowledged by memory"),
      blocks(&stats, "blocks", statistics::units::Count::get(),
             "Unit-stride blocks prepared, including faulting blocks"),
      splitElementBlocks(&stats, "splitElementBlocks",
                         statistics::units::Count::get(),
                         "One-element blocks crossing a line, page or "
                         "address-space boundary"),
      blockBytes(&stats, "blockBytes", statistics::units::Byte::get(),
                 "Bytes per prepared unit-stride block"),
      blockVrfAccesses(&stats, "blockVrfAccesses",
                       statistics::units::Count::get(),
                       "Local VRF word accesses per prepared block"),
      memoryRequests(&stats, "memoryRequests", statistics::units::Count::get(),
                     "Logical block requests accepted by memory backend"),
      memoryRetries(&stats, "memoryRetries", statistics::units::Count::get(),
                    "Logical block attempts rejected by backend"),
      memoryTicks(&stats, "memoryTicks", statistics::units::Tick::get(),
                  "Ticks from ready block to terminal memory response"),
      vrfReadsGranted(&stats, "vrfReadsGranted",
                      statistics::units::Count::get(),
                      "Store word reads granted by local VRF"),
      vrfWritesGranted(&stats, "vrfWritesGranted",
                       statistics::units::Count::get(),
                       "Load word writebacks granted by local VRF"),
      vrfGatherTicks(&stats, "vrfGatherTicks", statistics::units::Tick::get(),
                     "Ticks from store read publication to last response"),
      vrfScatterTicks(&stats, "vrfScatterTicks",
                      statistics::units::Tick::get(),
                      "Ticks from load write publication to last WriteAck"),
      owner(owner),
      mapper(mapper),
      parameters(parameters),
      sendMemory(std::move(send_memory)),
      queryBoundaries(std::move(query_boundaries)),
      wakeVrf(std::move(wake_vrf)),
      sendCompletion(std::move(send_completion)),
      progressEvent([this] { evaluate(); }, owner.name() + ".vlsu", false,
                    VpuProgressPriority)
{
    fatal_if(parameters.activeBlocks != 1 || !parameters.maxBlockBytes ||
                 !parameters.vrfInputsPerLane,
             "VLSU supports one active block with positive byte and VRF "
             "input limits");
    fatal_if(address_bits != 32 && address_bits != 64,
             "VLSU requires a 32-bit or 64-bit target address width");
    addressMask = address_bits == 64 ? std::numeric_limits<Addr>::max()
                                     : Addr{0xffffffff};
    fatal_if(!sendMemory || !queryBoundaries || !wakeVrf || !sendCompletion,
             "VLSU requires memory, boundary, VRF and completion endpoints");

    // Un bloque empieza en un offset múltiplo del elemento, que puede quedar
    // en mitad de una palabra; por eso puede tocar una palabra más.
    const auto word_bytes = mapper.geometry().laneWordBytes;
    const uint32_t max_words = parameters.maxBlockBytes / word_bytes + 2;
    const uint32_t byte_bucket =
        std::max<uint32_t>(1, (parameters.maxBlockBytes + 15) / 16);
    blockBytes.init(0, parameters.maxBlockBytes, byte_bucket);
    blockVrfAccesses.init(1, max_words, 1);
}

AraVLSU::~AraVLSU()
{
    // Destruir el módulo no sustituye drain: pueden quedar respuestas que
    // todavía lo referencien, aunque ahora no haya un evento programado.
    panic_if(!isIdle(), "Destroying VLSU before its work has drained");
}

void
AraVLSU::validateTask(const MemoryTask &task) const
{
    // El sequencer entrega una operación ya admitida. Una incoherencia aquí
    // es un error interno del protocolo, no una excepción del programa RVV.
    const auto &unit = task.task;
    const auto &config = task.config;
    const auto &memory = task.memory;
    panic_if(!unit.valid() || unit.unit != VectorUnitClass::Vlsu,
             "Invalid memory task identity or unit");
    panic_if(!VectorSupport::supportsConfiguration(config) ||
                 !unit.elements.fitsWithin(config.vstart, config.vl),
             "Invalid memory task configuration or active elements");
    panic_if(!VectorSupport::supportsMemory(memory, config.sewBits),
             "Unsupported operation in an admitted memory task");
    panic_if(VectorSupport::elementBytes(memory.elementWidthBits) >
                 parameters.maxBlockBytes,
             "Memory element exceeds the configured block byte limit");
    panic_if(task.requestorId == Request::invldRequestorId,
             "Memory task has no requestor identity");

    // Comprobar en 64 bits antes de usar rangos de bytes de 32 bits evita
    // aceptar un rango truncado al multiplicar por el tamaño de elemento.
    const auto element_bytes =
        VectorSupport::elementBytes(memory.elementWidthBits);
    const uint64_t offset =
        uint64_t{unit.elements.firstElement} * element_bytes;
    const uint64_t size = uint64_t{unit.elements.elementCount} * element_bytes;
    const uint64_t end = offset + size;
    panic_if(end > std::numeric_limits<uint32_t>::max() ||
                 task.dataRange.offset != offset ||
                 task.dataRange.size != size,
             "Memory byte range does not match its elements");
    // LMUL fraccionario usa un registro contenedor, pero sólo permite
    // acceder a su fracción inicial. regCount no expresa esa capacidad.
    const auto capacity =
        VectorSupport::groupBytes(mapper.geometry().vlenBytes, config.lmul);
    const auto registers = VectorSupport::groupRegisterCount(config.lmul);
    panic_if(uint64_t{config.vl} * element_bytes > capacity ||
                 end > capacity || !memory.dataReg.naturallyAligned() ||
                 memory.dataReg.regCount != registers,
             "Memory task exceeds its architectural register group");
}

TransferResult
AraVLSU::acceptTask(const MemoryTask &task)
{
    // Retry no reserva recursos ni conserva referencias a la tarea recibida.
    if (active) {
        ++stats.retries;
        return TransferResult::Retry;
    }
    validateTask(task);
    // Las fronteras pertenecen al contexto y no cambian durante la tarea:
    // se consultan una vez, antes de formar el primer bloque.
    const auto boundaries = queryBoundaries(task.task.key.command.contextId);
    panic_if(!boundaries.valid(), "VLSU received invalid memory boundaries");
    active.emplace();
    active->task = task;
    active->limits = {parameters.maxBlockBytes, boundaries, addressMask};
    active->elementBytes =
        VectorSupport::elementBytes(task.memory.elementWidthBits);
    if (task.memory.direction == MemoryDirection::Load) {
        ++loadTasks;
    } else {
        ++storeTasks;
    }
    stats.accept(task.task.elements.elementCount, task.dataRange.size);
    active->nextElement = task.task.elements.firstElement;
    wakeup();
    return TransferResult::Accepted;
}

void
AraVLSU::prepareBlock()
{
    auto &state = *active;
    panic_if(state.block ||
                 state.nextElement >= state.task.task.elements.end() ||
                 state.nextRequestId == InvalidRequestId,
             "Invalid VLSU block progress or exhausted request identifiers");
    const auto plan = planMemoryBlock(
        state.task.memory.base, state.elementBytes, state.nextElement,
        state.task.task.elements.end(), state.limits);

    state.block.emplace();
    auto &block = *state.block;
    auto &request = block.request;
    request.taskKey = state.task.task.key;
    request.requestId = state.nextRequestId++;
    request.direction = state.task.memory.direction;
    request.registerRef = state.task.memory.dataReg;
    request.elements = plan.elements;
    request.elementBytes = state.elementBytes;
    // El rango del VRF empieza en firstElement * E: vstart elige el primer
    // índice, pero no desplaza la base del vector ni el grupo de registros.
    request.dataRange =
        ByteRange{plan.elements.firstElement * state.elementBytes, plan.bytes};
    request.virtualAddress = plan.virtualAddress;
    request.size = plan.bytes;
    // Sin predicación: todos los bytes del bloque están habilitados.
    request.byteEnable.assign(plan.bytes, 1);
    request.pc = state.task.pc;
    request.requestorId = state.task.requestorId;
    block.splitElement = plan.splitElement;

    // El mapper es la única autoridad de ubicación. Devuelve una entrada por
    // palabra local, en orden creciente de bytes; cada una se convierte en
    // un acceso con identidad propia. Comprobar que los rangos son contiguos
    // garantiza que cada byte del bloque se cubre exactamente una vez.
    const auto mapping =
        mapper.map(request.registerRef, request.dataRange, request.byteEnable);
    panic_if(mapping.empty() ||
                 mapping.size() >= InvalidVrfAccessId - state.nextAccessId,
             "VLSU block has no VRF mapping or exhausted access identifiers");
    block.firstAccessId = state.nextAccessId;
    uint32_t next_offset = request.dataRange.offset;
    for (const auto &fragment : mapping) {
        panic_if(fragment.originalRange.offset != next_offset ||
                     !fragment.originalRange.valid() ||
                     fragment.laneId >= mapper.geometry().numLanes,
                 "VRF mapping of a VLSU block has gaps or overlaps");
        BlockVrfAccess access;
        access.laneId = fragment.laneId;
        access.access = VrfAccess{
            VrfAccessKey{request.taskKey,
                         VrfRequester{VrfRequesterKind::Vlsu, std::nullopt},
                         state.nextAccessId++},
            request.registerRef, fragment.originalRange,
            ByteEnable(fragment.originalRange.size, 1)};
        access.bufferOffset = next_offset - request.dataRange.offset;
        next_offset = fragment.originalRange.end();
        block.accesses.push_back(std::move(access));
    }
    panic_if(next_offset != request.dataRange.end(),
             "VRF mapping does not cover the whole VLSU block");

    ++blocks;
    if (block.splitElement) {
        ++splitElementBlocks;
    }
    blockBytes.sample(plan.bytes);
    blockVrfAccesses.sample(block.accesses.size());
    block.phaseStart = curTick();

    // Reservar el buffer del bloque antes de cualquier acceso. Una carga lo
    // recibe del backend; un store lo llena con las lecturas del VRF y no
    // se envía a memoria hasta tenerlo completo.
    if (request.direction == MemoryDirection::Load) {
        state.phase = Phase::SendMemory;
    } else {
        request.storeData.assign(plan.bytes, 0);
        state.phase = Phase::PublishReads;
    }
}

void
AraVLSU::publishAccesses(bool write)
{
    auto &block = *active->block;
    panic_if(block.pendingAccesses || (write && !block.load),
             "VLSU publishes VRF accesses with pending work or no load data");
    // Todas las ofertas del bloque son elegibles desde el siguiente flanco.
    // Cada LRF sólo ve las de su lane, a través de vrfInputsPerLane entradas.
    const Tick eligible_at = vpuClockEdge(owner, Cycles(1));
    std::vector<bool> lanes(mapper.geometry().numLanes, false);
    for (auto &access : block.accesses) {
        panic_if(access.phase != AccessPhase::Idle || access.request,
                 "VLSU word access published more than once");
        if (write) {
            // Copia la palabra local del buffer de carga. LoadData conserva
            // el bloque completo hasta el último WriteAck.
            const auto first = block.load->data.begin() + access.bufferOffset;
            access.request = VrfWriteRequest{
                access.access,
                ByteBuffer(first, first + access.access.range.size)};
        } else {
            access.request = VrfReadRequest{access.access};
        }
        access.eligibleAt = eligible_at;
        access.publishedAt = curTick();
        access.phase = AccessPhase::Offered;
        lanes[access.laneId] = true;
    }
    block.pendingAccesses = block.accesses.size();
    block.phaseStart = curTick();
    if (write) {
        active->phase = Phase::ScatterWrites;
    } else {
        active->phase = Phase::GatherReads;
    }
    // Wakeup sólo programa arbitraje: ningún LRF responde dentro de él.
    for (LaneId lane_id = 0; lane_id < lanes.size(); ++lane_id) {
        if (lanes[lane_id]) {
            wakeVrf(lane_id);
        }
    }
}

std::optional<VrfOffer>
AraVLSU::peekOffer(LaneId lane_id, uint32_t input) const
{
    if (!active || !active->block ||
        (active->phase != Phase::GatherReads &&
         active->phase != Phase::ScatterWrites)) {
        return std::nullopt;
    }
    // La entrada input muestra el input-ésimo acceso aún no concedido de
    // esta lane, en orden de bytes. Al conceder uno, los siguientes avanzan
    // de entrada; la petición y su dirección en memoria no cambian.
    uint32_t seen = 0;
    for (const auto &access : active->block->accesses) {
        if (access.laneId != lane_id || access.phase != AccessPhase::Offered) {
            continue;
        }
        if (seen == input) {
            panic_if(!access.request, "VLSU offer has no stable request");
            return VrfOffer{&*access.request, access.eligibleAt,
                            access.publishedAt};
        }
        ++seen;
    }
    return std::nullopt;
}

VrfRequestPort
AraVLSU::vrfPort(LaneId lane_id, uint32_t input)
{
    panic_if(lane_id >= mapper.geometry().numLanes ||
                 input >= parameters.vrfInputsPerLane,
             "Invalid VLSU VRF port destination or input");
    return {[this, lane_id, input] { return peekOffer(lane_id, input); },
            [this](const VrfAccessKey &key) { recvVrfGrant(key); }};
}

AraVLSU::BlockVrfAccess &
AraVLSU::findAccess(const VrfAccessKey &key, AccessPhase phase)
{
    panic_if(!active || !active->block, "VLSU VRF event without a block");
    auto &block = *active->block;
    // accessId es consecutivo dentro del bloque: el índice localiza el
    // acceso y la clave completa detecta respuestas ajenas o duplicadas.
    panic_if(key.accessId < block.firstAccessId ||
                 key.accessId - block.firstAccessId >= block.accesses.size(),
             "VLSU VRF event for an access outside the active block");
    auto &access = block.accesses[key.accessId - block.firstAccessId];
    panic_if(access.access.key != key || access.phase != phase,
             "VLSU VRF event has wrong identity or access phase");
    return access;
}

void
AraVLSU::recvVrfGrant(const VrfAccessKey &key)
{
    auto &access = findAccess(key, AccessPhase::Offered);
    panic_if(access.eligibleAt > curTick(), "Early VLSU VRF grant");
    // El LRF ya copió la petición; se conserva hasta la respuesta.
    access.phase = AccessPhase::Granted;
    if (active->phase == Phase::GatherReads) {
        ++vrfReadsGranted;
    } else {
        panic_if(active->phase != Phase::ScatterWrites,
                 "VLSU VRF grant in a phase without offers");
        ++vrfWritesGranted;
    }
}

bool
AraVLSU::hasReadyWork() const
{
    // En GATHER, SCATTER y WAIT_MEMORY hay operaciones aceptadas: sólo sus
    // respuestas pueden avanzar el estado. Sondear duplicaría sus efectos.
    if (!active) {
        return false;
    }
    return active->phase != Phase::GatherReads &&
           active->phase != Phase::ScatterWrites &&
           active->phase != Phase::WaitMemory;
}

void
AraVLSU::wakeup()
{
    // Incluso una respuesta inmediata continúa en el siguiente ciclo.
    if (hasReadyWork() && !progressEvent.scheduled()) {
        owner.schedule(progressEvent, vpuClockEdge(owner, Cycles(1)));
    }
}

void
AraVLSU::handleMemoryTransfer(TransferResult result)
{
    switch (result) {
        case TransferResult::Accepted:
            ++memoryRequests;
            // Una respuesta síncrona puede haber avanzado ya la fase.
            // No restaurar WAIT ni sobrescribir el estado de esa respuesta.
            break;
        case TransferResult::Retry:
            ++memoryRetries;
            // El backend rechazó la entrega: se conserva el mismo descriptor
            // y buffer. Responder y rechazar a la vez rompe el contrato.
            panic_if(active->phase != Phase::WaitMemory,
                     "Memory responded to a VLSU block it rejected");
            active->phase = Phase::SendMemory;
            break;
        default:
            panic("Invalid VLSU transfer result");
    }
}

void
AraVLSU::evaluate()
{
    if (!active) {
        return;
    }
    auto &state = *active;
    // One transition per event. VRF offers wait for arbitration without
    // polling. Memory keeps the Accepted/Retry protocol, installing WAIT
    // before sending because backend callbacks may be synchronous.
    switch (state.phase) {
        case Phase::Prepare:
            prepareBlock();
            break;
        case Phase::PublishReads:
            publishAccesses(false);
            break;
        case Phase::SendMemory:
            state.phase = Phase::WaitMemory;
            handleMemoryTransfer(sendMemory(state.block->request));
            break;
        case Phase::PublishWrites:
            publishAccesses(true);
            break;
        case Phase::NextBlock:
            closeBlock();
            if (state.nextElement == state.task.task.elements.end()) {
                finish();
                return;
            }
            state.phase = Phase::Prepare;
            break;
        case Phase::MemoryFault:
            finish();
            return;
        case Phase::GatherReads:
        case Phase::WaitMemory:
        case Phase::ScatterWrites:
            break;
    }
    wakeup();
}

void
AraVLSU::recvVrfReadResponse(const ReadResponse &response)
{
    panic_if(!active || active->phase != Phase::GatherReads,
             "Unexpected VLSU VRF read response");
    auto &block = *active->block;
    auto &access = findAccess(response.key, AccessPhase::Granted);
    panic_if(response.data.size() != access.access.range.size,
             "VLSU VRF read response has wrong size");
    // La posición procede del acceso, no del orden de llegada. Los bytes se
    // capturan una vez: un retry posterior del backend no relee el VRF.
    std::copy(response.data.begin(), response.data.end(),
              block.request.storeData.begin() + access.bufferOffset);
    access.request.reset();
    access.phase = AccessPhase::Done;
    --block.pendingAccesses;
    if (!block.pendingAccesses) {
        // Buffer completo: el bloque puede enviarse a memoria.
        vrfGatherTicks += curTick() - block.phaseStart;
        block.phaseStart = curTick();
        active->phase = Phase::SendMemory;
        wakeup();
    }
}

void
AraVLSU::recvVrfWriteAck(const WriteAck &ack)
{
    panic_if(!active || active->phase != Phase::ScatterWrites,
             "Unexpected VLSU VRF write acknowledgement");
    auto &block = *active->block;
    auto &access = findAccess(ack.key, AccessPhase::Granted);
    access.request.reset();
    access.phase = AccessPhase::Done;
    --block.pendingAccesses;
    if (!block.pendingAccesses) {
        // Un WriteAck sólo cierra su palabra; el bloque termina con el último.
        vrfScatterTicks += curTick() - block.phaseStart;
        active->phase = Phase::NextBlock;
        wakeup();
    }
}

void
AraVLSU::recvMemoryResponse(const VectorMemoryResponse &response)
{
    // La asociación se comprueba por clave completa y fase, aunque sólo haya
    // un bloque activo. Así se detectan respuestas ajenas o duplicadas.
    panic_if(!active || !active->block || active->phase != Phase::WaitMemory,
             "Unexpected VLSU memory response");
    auto &block = *active->block;
    const auto &request = block.request;
    panic_if(response.taskKey != request.taskKey ||
                 response.requestId != request.requestId,
             "VLSU memory response has wrong identity");
    memoryTicks += curTick() - block.phaseStart;
    switch (response.status) {
        case MemoryResponseStatus::LoadData:
            panic_if(request.direction != MemoryDirection::Load ||
                         response.data.size() != request.size ||
                         response.fault,
                     "Invalid load response");
            // Los metadatos de destino proceden de la petición conservada.
            // Todavía falta repartir los bytes y recibir todos los WriteAck.
            block.load = LoadData{request.taskKey,      request.requestId,
                                  request.registerRef,  request.elements,
                                  request.elementBytes, request.dataRange,
                                  response.data};
            active->phase = Phase::PublishWrites;
            break;
        case MemoryResponseStatus::StoreAck:
            panic_if(request.direction != MemoryDirection::Store ||
                         !response.data.empty() || response.fault,
                     "Invalid store acknowledgement");
            active->phase = Phase::NextBlock;
            break;
        case MemoryResponseStatus::Fault:
            // El backend sólo responde Fault cuando ha cerrado sus accesos.
            // El índice es firstElement (véase VectorMemoryRequest); la
            // dirección pertenece al bloque, con la resta enmascarada al
            // ancho del objetivo.
            panic_if(!response.data.empty() || !response.fault ||
                         response.fault->fault == NoFault ||
                         !response.fault->address ||
                         !response.fault->elementIndex ||
                         *response.fault->elementIndex !=
                             request.elements.firstElement,
                     "Incomplete or mismatched memory fault");
            panic_if(*response.fault->address > addressMask ||
                         ((*response.fault->address - request.virtualAddress) &
                          addressMask) >= request.size,
                     "Memory fault address is outside the active block");
            active->fault = response.fault;
            // No se escribe una carga fallida ni se inicia otro bloque.
            // El sequencer usará este elementIndex como finalVstart.
            active->phase = Phase::MemoryFault;
            break;
        default:
            panic("Invalid memory response status");
    }
    wakeup();
}

void
AraVLSU::closeBlock()
{
    // Sólo el último WriteAck o StoreAck llega aquí: ningún efecto pendiente
    // del bloque anterior se solapa con la preparación del siguiente.
    auto &state = *active;
    panic_if(!state.block || state.block->pendingAccesses,
             "Cannot close a VLSU block with pending VRF accesses");
    const auto &request = state.block->request;
    if (request.direction == MemoryDirection::Load) {
        loadElements += request.elements.elementCount;
        loadBytes += request.size;
    } else {
        storeElements += request.elements.elementCount;
        storeBytes += request.size;
    }
    state.nextElement = request.elements.end();
    state.block.reset();
}

void
AraVLSU::finish()
{
    // Un fault llega con el bloque aún presente pero sin accesos VRF: una
    // carga no publica escrituras y un store termina sus lecturas antes de
    // enviar memoria.
    panic_if(!active ||
                 (active->phase == Phase::MemoryFault &&
                  (!active->block || active->block->pendingAccesses)) ||
                 (active->phase != Phase::MemoryFault &&
                  (active->block ||
                   active->nextElement != active->task.task.elements.end())),
             "Cannot finish a VLSU task with unfinished blocks");
    const UnitCompletion completion{active->task.task.key,
                                    active->fault
                                        ? UnitCompletionStatus::MemoryFault
                                        : UnitCompletionStatus::Success,
                                    active->fault};
    // Construir la finalización antes de liberar el estado. Al liberar la
    // capacidad antes del callback se permite admitir una nueva tarea en él.
    stats.complete(active->fault.has_value());
    active.reset();
    sendCompletion(completion);
}

} // namespace gem5::vector_engine
