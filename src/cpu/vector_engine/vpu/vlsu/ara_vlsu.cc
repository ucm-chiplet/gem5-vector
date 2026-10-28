// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/vlsu/ara_vlsu.hh"

#include <limits>
#include <utility>
#include <variant>

#include "base/logging.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

AraVLSU::AraVLSU(ClockedObject &owner, const AddressMapper &mapper,
                 const VlsuParameters &parameters, unsigned address_bits,
                 MemorySender send_memory, VrfWakeup wake_vrf,
                 CompletionSender send_completion)
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
      memoryRequests(&stats, "memoryRequests", statistics::units::Count::get(),
                     "Logical requests accepted by memory backend"),
      memoryRetries(&stats, "memoryRetries", statistics::units::Count::get(),
                    "Logical request attempts rejected by backend"),
      vrfReadsGranted(&stats, "vrfReadsGranted",
                      statistics::units::Count::get(),
                      "Store reads granted by local VRF"),
      vrfWritesGranted(&stats, "vrfWritesGranted",
                       statistics::units::Count::get(),
                       "Load writebacks granted by local VRF"),
      owner(owner),
      mapper(mapper),
      parameters(parameters),
      sendMemory(std::move(send_memory)),
      wakeVrf(std::move(wake_vrf)),
      sendCompletion(std::move(send_completion)),
      progressEvent([this] { evaluate(); }, owner.name() + ".vlsu", false,
                    VpuProgressPriority)
{
    fatal_if(parameters.activeBlocks != 1 || parameters.blockRequests ||
                 !parameters.maxBlockBytes,
             "VLSU supports one active element; invalid block contract");
    fatal_if(address_bits != 32 && address_bits != 64,
             "VLSU requires a 32-bit or 64-bit target address width");
    addressMask = address_bits == 64 ? std::numeric_limits<Addr>::max()
                                     : Addr{0xffffffff};
    fatal_if(!sendMemory || !wakeVrf || !sendCompletion,
             "VLSU requires memory, VRF and completion endpoints");
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
    active.emplace();
    active->task = task;
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
AraVLSU::prepareElement()
{
    auto &state = *active;
    panic_if(state.element ||
                 state.nextElement >= state.task.task.elements.end() ||
                 state.nextRequestId == InvalidRequestId ||
                 state.nextAccessId == InvalidVrfAccessId,
             "Invalid VLSU element progress or exhausted identifiers");
    state.element.emplace();
    auto &element = *state.element;
    auto &request = element.request;
    request.taskKey = state.task.task.key;
    request.requestId = state.nextRequestId++;
    request.direction = state.task.memory.direction;
    request.registerRef = state.task.memory.dataReg;
    request.elements = ElementRange{state.nextElement, 1};
    // vstart limita qué elementos se ejecutan; no desplaza la base del vector.
    // La máscara aplica el ancho del objetivo a la suma sin signo.
    const auto element_bytes =
        VectorSupport::elementBytes(state.task.memory.elementWidthBits);
    request.dataRange =
        ByteRange{state.nextElement * element_bytes, element_bytes};
    request.virtualAddress =
        (state.task.memory.base + Addr{state.nextElement} * element_bytes) &
        addressMask;
    request.size = element_bytes;
    request.elementBytes = element_bytes;
    request.byteEnable.assign(element_bytes, 1);
    request.pc = state.task.pc;
    request.requestorId = state.task.requestorId;
    // El mapper es la única fuente del reparto por lane. La geometría
    // garantiza que el elemento cabe en una palabra de VRF.
    const auto mapping =
        mapper.map(request.registerRef, request.dataRange, request.byteEnable);
    panic_if(mapping.size() != 1 ||
                 mapping.front().originalRange != request.dataRange,
             "A VLSU element must belong to one VRF word");
    element.laneId = mapping.front().laneId;
    // Guardar la ubicación aquí: pertenece a este elemento, no a la forma
    // general de una petición que en el futuro podría agrupar varios.
    // La lectura del store y la escritura de la carga usan la misma forma
    // de acceso, pero cada elemento obtiene un accessId nuevo.
    element.vrfAccess = VrfAccess{
        VrfAccessKey{request.taskKey,
                     VrfRequester{VrfRequesterKind::Vlsu, std::nullopt},
                     state.nextAccessId++},
        request.registerRef, request.dataRange, request.byteEnable};
    // Una carga empieza en memoria; un store debe leer antes sus bytes del
    // VRF.
    if (request.direction == MemoryDirection::Load) {
        state.phase = Phase::SendMemory;
    } else {
        state.phase = Phase::SendRead;
    }
}

VrfRequestPort
AraVLSU::vrfPort(LaneId lane_id)
{
    panic_if(lane_id >= mapper.geometry().numLanes,
             "Invalid VLSU VRF port destination");
    return {[this, lane_id]() -> std::optional<VrfOffer> {
                if (!active || !active->element ||
                    active->element->laneId != lane_id ||
                    (active->phase != Phase::OfferRead &&
                     active->phase != Phase::OfferWrite)) {
                    return std::nullopt;
                }
                const auto &element = *active->element;
                panic_if(!element.vrfRequest,
                         "VLSU offer has no stable request");
                return VrfOffer{&*element.vrfRequest, element.vrfEligibleAt,
                                element.vrfPublishedAt};
            },
            [this](const VrfAccessKey &key) { recvVrfGrant(key); }};
}

void
AraVLSU::recvVrfGrant(const VrfAccessKey &key)
{
    panic_if(!active || !active->element || !active->element->vrfRequest ||
                 active->element->vrfEligibleAt > curTick() ||
                 active->element->vrfAccess.key != key,
             "Unexpected VLSU VRF grant");
    if (active->phase == Phase::OfferRead) {
        active->phase = Phase::WaitRead;
        ++vrfReadsGranted;
    } else {
        panic_if(active->phase != Phase::OfferWrite,
                 "Duplicate VLSU VRF grant or wrong phase");
        active->phase = Phase::WaitWrite;
        ++vrfWritesGranted;
    }
}

bool
AraVLSU::hasReadyWork() const
{
    // En WAIT ya hay una operación aceptada: sólo su respuesta puede avanzar
    // el estado. Sondear o reenviar en esa fase duplicaría sus efectos.
    if (!active) {
        return false;
    }
    return active->phase != Phase::OfferRead &&
           active->phase != Phase::OfferWrite &&
           active->phase != Phase::WaitRead &&
           active->phase != Phase::WaitMemory &&
           active->phase != Phase::WaitWrite;
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
AraVLSU::handleTransfer(TransferResult result, Phase send, Phase wait)
{
    switch (result) {
        case TransferResult::Accepted:
            if (send == Phase::SendMemory) {
                ++memoryRequests;
            }
            // Una respuesta síncrona puede haber avanzado ya la fase.
            // No restaurar WAIT ni sobrescribir el estado de esa respuesta.
            break;
        case TransferResult::Retry:
            if (send == Phase::SendMemory) {
                ++memoryRetries;
            }
            // El receptor rechazó la entrega: conserva el mensaje y vuelve
            // a intentarlo. Responder y rechazar a la vez rompe el contrato.
            panic_if(active->phase != wait,
                     "A receiver responded to a VLSU access it rejected");
            active->phase = send;
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
    // polling. Memory keeps the existing Accepted/Retry protocol, installing
    // WAIT before sending because backend callbacks may be synchronous.
    switch (state.phase) {
        case Phase::Prepare:
            prepareElement();
            break;
        case Phase::SendRead:
            // Store data reception is bounded by this element's request size.
            state.element->vrfRequest =
                VrfReadRequest{state.element->vrfAccess};
            state.element->vrfEligibleAt = vpuClockEdge(owner, Cycles(1));
            state.element->vrfPublishedAt = curTick();
            state.phase = Phase::OfferRead;
            wakeVrf(state.element->laneId);
            break;
        case Phase::SendMemory:
            state.phase = Phase::WaitMemory;
            handleTransfer(sendMemory(state.element->request),
                           Phase::SendMemory, Phase::WaitMemory);
            break;
        case Phase::SendWrite: {
            panic_if(!state.element->load, "Load writeback has no data");
            // LoadData owns the bytes through contention and acknowledgement.
            state.element->vrfRequest = VrfWriteRequest{
                state.element->vrfAccess, state.element->load->data};
            state.element->vrfEligibleAt = vpuClockEdge(owner, Cycles(1));
            state.element->vrfPublishedAt = curTick();
            state.phase = Phase::OfferWrite;
            wakeVrf(state.element->laneId);
            break;
        }
        case Phase::NextElement:
            // Sólo WriteAck o StoreAck habilitan esta fase: ningún efecto
            // pendiente del elemento anterior se solapa con el siguiente.
            if (state.task.memory.direction == MemoryDirection::Load) {
                ++loadElements;
                loadBytes += state.element->request.size;
            } else {
                ++storeElements;
                storeBytes += state.element->request.size;
            }
            state.element.reset();
            ++state.nextElement;
            if (state.nextElement == state.task.task.elements.end()) {
                finish();
                return;
            }
            state.phase = Phase::Prepare;
            break;
        case Phase::MemoryFault:
            finish();
            return;
        case Phase::OfferRead:
        case Phase::OfferWrite:
        case Phase::WaitRead:
        case Phase::WaitMemory:
        case Phase::WaitWrite:
            break;
    }
    wakeup();
}

void
AraVLSU::recvVrfReadResponse(const ReadResponse &response)
{
    panic_if(!active || !active->element || active->phase != Phase::WaitRead,
             "Unexpected VLSU VRF read response");
    auto &element = *active->element;
    panic_if(response.key != element.vrfAccess.key ||
                 response.data.size() != element.request.size,
             "VLSU VRF read response has wrong identity or size");
    // Capturar los bytes una vez: un retry posterior del backend no requiere
    // releer el registro ni permite cambiar el valor que se almacenará.
    element.request.storeData = response.data;
    element.vrfRequest.reset();
    active->phase = Phase::SendMemory;
    wakeup();
}

void
AraVLSU::recvVrfWriteAck(const WriteAck &ack)
{
    panic_if(!active || !active->element || active->phase != Phase::WaitWrite,
             "Unexpected VLSU VRF write acknowledgement");
    panic_if(ack.key != active->element->vrfAccess.key,
             "VLSU VRF write acknowledgement has wrong identity");
    active->element->vrfRequest.reset();
    active->phase = Phase::NextElement;
    wakeup();
}

void
AraVLSU::recvMemoryResponse(const VectorMemoryResponse &response)
{
    // La asociación se comprueba por clave completa y fase, aunque sólo haya
    // un elemento activo. Así se detectan respuestas ajenas o duplicadas.
    panic_if(!active || !active->element || active->phase != Phase::WaitMemory,
             "Unexpected VLSU memory response");
    auto &element = *active->element;
    const auto &request = element.request;
    panic_if(response.taskKey != request.taskKey ||
                 response.requestId != request.requestId,
             "VLSU memory response has wrong identity");
    switch (response.status) {
        case MemoryResponseStatus::LoadData:
            panic_if(request.direction != MemoryDirection::Load ||
                         response.data.size() != request.size ||
                         response.fault,
                     "Invalid load response");
            // Los metadatos de destino proceden de la petición conservada.
            // Todavía falta escribir estos bytes en el LRF y recibir su ack.
            element.load = LoadData{request.taskKey,      request.requestId,
                                    request.registerRef,  request.elements,
                                    request.elementBytes, request.dataRange,
                                    response.data};
            active->phase = Phase::SendWrite;
            break;
        case MemoryResponseStatus::StoreAck:
            panic_if(request.direction != MemoryDirection::Store ||
                         !response.data.empty() || response.fault,
                     "Invalid store acknowledgement");
            active->phase = Phase::NextElement;
            break;
        case MemoryResponseStatus::Fault:
            // El backend sólo responde Fault cuando ha cerrado sus accesos.
            // El fault puede apuntar a un fragmento dentro del elemento;
            // la resta enmascarada contempla el ancho del objetivo.
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
                     "Memory fault address is outside the active element");
            active->fault = response.fault;
            // No se escribe una carga fallida ni se inicia otro elemento.
            // El sequencer usará este elementIndex como finalVstart.
            active->phase = Phase::MemoryFault;
            break;
        default:
            panic("Invalid memory response status");
    }
    wakeup();
}

void
AraVLSU::finish()
{
    panic_if(!active ||
                 (active->phase != Phase::MemoryFault &&
                  (active->element ||
                   active->nextElement != active->task.task.elements.end())),
             "Cannot finish a VLSU task with unfinished elements");
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
