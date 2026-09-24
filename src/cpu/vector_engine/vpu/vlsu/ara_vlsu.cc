// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/vlsu/ara_vlsu.hh"

#include <limits>
#include <utility>
#include <variant>

#include "base/logging.hh"

namespace gem5::vector_engine
{

AraVLSU::AraVLSU(ClockedObject &owner, const AddressMapper &mapper,
                 unsigned address_bits, MemorySender send_memory,
                 ReadSender send_read, WriteSender send_write,
                 CompletionSender send_completion)
    : owner(owner),
      mapper(mapper),
      sendMemory(std::move(send_memory)),
      sendRead(std::move(send_read)),
      sendWrite(std::move(send_write)),
      sendCompletion(std::move(send_completion)),
      progressEvent([this] { evaluate(); }, owner.name() + ".vlsu")
{
    fatal_if(address_bits != 32 && address_bits != 64,
             "VLSU requires a 32-bit or 64-bit target address width");
    addressMask = address_bits == 64 ? std::numeric_limits<Addr>::max()
                                     : Addr{0xffffffff};
    fatal_if(!sendMemory || !sendRead || !sendWrite || !sendCompletion,
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
    panic_if(!isValid(config.lmul) || config.sewBits != 32 || config.masked ||
                 !unit.elements.fitsWithin(config.vstart, config.vl),
             "Invalid memory task configuration or active elements");
    panic_if(
        (memory.direction != MemoryDirection::Load &&
         memory.direction != MemoryDirection::Store) ||
            !std::holds_alternative<UnitStrideAddress>(memory.addressing) ||
            memory.elementWidthBits != 32 || memory.fieldCount != 1 ||
            memory.faultOnlyFirst,
        "Unsupported operation in an admitted memory task");
    panic_if(task.requestorId == Request::invldRequestorId,
             "Memory task has no requestor identity");

    // Comprobar en 64 bits antes de usar rangos de bytes de 32 bits evita
    // aceptar un rango que se haya truncado al multiplicar por cuatro.
    const uint64_t offset = uint64_t{unit.elements.firstElement} * 4;
    const uint64_t size = uint64_t{unit.elements.elementCount} * 4;
    const uint64_t end = offset + size;
    panic_if(end > std::numeric_limits<uint32_t>::max() ||
                 task.dataRange.offset != offset ||
                 task.dataRange.size != size,
             "Memory byte range does not match its elements");
    // LMUL fraccionario usa un registro contenedor, pero sólo permite
    // acceder a su fracción inicial. regCount no expresa esa capacidad.
    const uint64_t vlen_bytes = mapper.geometry().vlenBytes;
    const int exponent = static_cast<int>(config.lmul);
    const uint64_t capacity =
        exponent >= 0 ? vlen_bytes << exponent : vlen_bytes >> -exponent;
    const unsigned registers = exponent > 0 ? 1U << exponent : 1U;
    panic_if(uint64_t{config.vl} * 4 > capacity || end > capacity ||
                 !memory.dataReg.naturallyAligned() ||
                 memory.dataReg.regCount != registers,
             "Memory task exceeds its architectural register group");
}

TransferResult
AraVLSU::acceptTask(const MemoryTask &task)
{
    // Retry no reserva recursos ni conserva referencias a la tarea recibida.
    if (active) {
        return TransferResult::Retry;
    }
    validateTask(task);
    active.emplace();
    active->task = task;
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
    request.elementIndex = state.nextElement;
    // vstart limita qué elementos se ejecutan; no desplaza la base del vector.
    // La máscara aplica el ancho del objetivo a la suma sin signo.
    request.dataRange = ByteRange{state.nextElement * 4, 4};
    request.virtualAddress =
        (state.task.memory.base + Addr{state.nextElement} * 4) & addressMask;
    request.size = 4;
    request.byteEnable.assign(4, 1);
    request.pc = state.task.pc;
    request.requestorId = state.task.requestorId;
    // El mapper es la única fuente del reparto por lane. La geometría del
    // baseline garantiza que estos cuatro bytes caben en una palabra de VRF.
    const auto mapping =
        mapper.map(request.registerRef, request.dataRange, request.byteEnable);
    panic_if(mapping.size() != 1 ||
                 mapping.front().originalRange != request.dataRange,
             "A VLSU element must belong to one VRF word");
    request.laneId = mapping.front().laneId;
    // El emisor es Vlsu, sin lane propia; request.laneId es el destino.
    // La lectura del store y la escritura de la carga usan la misma forma
    // de acceso, pero cada elemento obtiene un accessId nuevo.
    element.vrfAccess = VrfAccess{
        VrfAccessKey{request.taskKey,
                     VrfRequester{VrfRequesterKind::Vlsu, std::nullopt},
                     state.nextAccessId++},
        request.registerRef, request.dataRange, request.byteEnable};
    state.phase = request.direction == MemoryDirection::Load
                      ? Phase::SendMemory
                      : Phase::SendRead;
}

bool
AraVLSU::hasReadyWork() const
{
    // En WAIT ya hay una operación aceptada: sólo su respuesta puede avanzar
    // el estado. Sondear o reenviar en esa fase duplicaría sus efectos.
    if (!active) {
        return false;
    }
    return active->phase != Phase::WaitRead &&
           active->phase != Phase::WaitMemory &&
           active->phase != Phase::WaitWrite;
}

void
AraVLSU::wakeup()
{
    // Incluso una respuesta inmediata continúa en el siguiente ciclo.
    if (hasReadyWork() && !progressEvent.scheduled()) {
        owner.schedule(progressEvent, owner.clockEdge(Cycles(1)));
    }
}

void
AraVLSU::handleTransfer(TransferResult result, Phase send, Phase wait)
{
    switch (result) {
        case TransferResult::Accepted:
            // Una respuesta síncrona puede haber avanzado ya la fase.
            // No restaurar WAIT ni sobrescribir el estado de esa respuesta.
            break;
        case TransferResult::Retry:
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
    // Se procesa una fase por evento. Antes de cada envío se habilita WAIT
    // para que el receptor pueda responder dentro de la llamada. Si rechaza
    // el mensaje, handleTransfer restaura SEND con los mismos datos y clave.
    switch (state.phase) {
        case Phase::Prepare:
            prepareElement();
            break;
        case Phase::SendRead: {
            const VrfReadRequest request{state.element->vrfAccess};
            state.phase = Phase::WaitRead;
            handleTransfer(sendRead(state.element->request.laneId, request),
                           Phase::SendRead, Phase::WaitRead);
            break;
        }
        case Phase::SendMemory:
            state.phase = Phase::WaitMemory;
            handleTransfer(sendMemory(state.element->request),
                           Phase::SendMemory, Phase::WaitMemory);
            break;
        case Phase::SendWrite: {
            panic_if(!state.element->load, "Load writeback has no data");
            const VrfWriteRequest request{state.element->vrfAccess,
                                          state.element->load->data};
            state.phase = Phase::WaitWrite;
            handleTransfer(sendWrite(state.element->request.laneId, request),
                           Phase::SendWrite, Phase::WaitWrite);
            break;
        }
        case Phase::NextElement:
            // Sólo WriteAck o StoreAck habilitan esta fase: ningún efecto
            // pendiente del elemento anterior se solapa con el siguiente.
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
                 response.data.size() != 4,
             "VLSU VRF read response has wrong identity or size");
    // Capturar los bytes una vez: un retry posterior del backend no requiere
    // releer el registro ni permite cambiar el valor que se almacenará.
    element.request.storeData = response.data;
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
                         response.data.size() != 4 || response.fault,
                     "Invalid load response");
            // Los metadatos de destino proceden de la petición conservada.
            // Todavía falta escribir estos bytes en el LRF y recibir su ack.
            element.load = LoadData{request.taskKey,     request.requestId,
                                    request.registerRef, request.elementIndex,
                                    request.laneId,      request.dataRange,
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
            // El fault puede apuntar a un fragmento dentro de los cuatro
            // bytes; la resta enmascarada contempla el ancho del objetivo.
            panic_if(!response.data.empty() || !response.fault ||
                         response.fault->fault == NoFault ||
                         !response.fault->address ||
                         !response.fault->elementIndex ||
                         *response.fault->elementIndex != request.elementIndex,
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
    active.reset();
    sendCompletion(completion);
}

} // namespace gem5::vector_engine
