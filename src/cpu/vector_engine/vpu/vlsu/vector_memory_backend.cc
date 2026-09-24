// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/vlsu/vector_memory_backend.hh"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "mem/page_table.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/system.hh"

namespace gem5::vector_engine
{

VectorMemoryBackend::MemoryPort::MemoryPort(VectorMemoryBackend &backend,
                                            const std::string &name)
    : RequestPort(name), backend(backend)
{}

bool
VectorMemoryBackend::MemoryPort::recvTimingResp(PacketPtr packet)
{
    return backend.recvTimingResp(packet);
}

void
VectorMemoryBackend::MemoryPort::recvReqRetry()
{
    backend.recvReqRetry();
}

VectorMemoryBackend::FragmentState::FragmentState(VectorMemoryBackend &backend,
                                                  std::size_t index,
                                                  uint32_t offset,
                                                  RequestPtr request)
    : backend(backend),
      index(index),
      offset(offset),
      request(std::move(request))
{}

void
VectorMemoryBackend::FragmentState::markDelayed()
{
    // La MMU retiene este callback. No se reintenta la traducción: finish()
    // notificará su resultado cuando esté disponible.
    panic_if(phase != Phase::Translating,
             "Delayed translation without a pending VLSU fragment");
}

void
VectorMemoryBackend::FragmentState::finish(const Fault &fault,
                                           const RequestPtr &request,
                                           ThreadContext *context,
                                           BaseMMU::Mode mode)
{
    backend.finishTranslation(*this, fault, request, context, mode);
}

VectorMemoryBackend::VectorMemoryBackend(ClockedObject &owner,
                                         unsigned address_bits,
                                         ContextResolver resolve_context,
                                         AccessFaultFactory access_fault,
                                         ResponseSender send_response)
    : owner(owner),
      resolveContext(std::move(resolve_context)),
      accessFault(std::move(access_fault)),
      sendResponse(std::move(send_response)),
      port(*this, owner.name() + ".vector_memory"),
      progressEvent([this] { evaluate(); }, owner.name() + ".vector_memory")
{
    fatal_if(FullSystem, "Vector memory backend only supports SE mode");
    fatal_if(address_bits != 32 && address_bits != 64,
             "Vector memory requires a 32-bit or 64-bit address width");
    addressMask = address_bits == 64 ? std::numeric_limits<Addr>::max()
                                     : Addr{0xffffffff};
    fatal_if(!resolveContext || !accessFault || !sendResponse,
             "Vector memory requires context, fault and response callbacks");
}

VectorMemoryBackend::~VectorMemoryBackend()
{
    // Un paquete aceptado pertenece al camino de memoria. Destruir aquí su
    // estado de retorno dejaría referencias inválidas en una respuesta futura.
    panic_if(!isIdle(), "Destroying vector memory before its work drained");
}

void
VectorMemoryBackend::validateRequest(const VectorMemoryRequest &request) const
{
    // Son invariantes de mensajes internos, no fallos de traducción/acceso.
    // El rango y los buffers describen un elemento entero, aun cuando después
    // se necesiten varios paquetes físicos para transportarlo.
    panic_if(!request.taskKey.valid() ||
                 request.requestId == InvalidRequestId ||
                 request.requestorId == Request::invldRequestorId,
             "Invalid vector memory request identity");
    panic_if(request.direction != MemoryDirection::Load &&
                 request.direction != MemoryDirection::Store,
             "Invalid vector memory direction");
    panic_if(request.size != 4 || !request.registerRef.valid() ||
                 !request.dataRange.valid() || request.dataRange.size != 4 ||
                 uint64_t{request.elementIndex} * 4 !=
                     request.dataRange.offset ||
                 request.virtualAddress > addressMask ||
                 request.byteEnable != ByteEnable(4, 1),
             "Invalid vector memory element descriptor");
    panic_if(request.storeData.size() !=
                 (request.direction == MemoryDirection::Store ? 4 : 0),
             "Invalid vector memory store buffer");
}

TransferResult
VectorMemoryBackend::acceptRequest(const VectorMemoryRequest &request)
{
    if (active) {
        return TransferResult::Retry;
    }
    validateRequest(request);
    auto *context = resolveContext(request.taskKey.command.contextId);
    panic_if(!context ||
                 context->contextId() != request.taskKey.command.contextId ||
                 !context->getMMUPtr() || !context->getProcessPtr() ||
                 !context->getSystemPtr(),
             "Vector memory request has no matching SE context");
    fatal_if(!context->getSystemPtr()->isTimingMode() || !port.isConnected(),
             "Vector memory requires a connected timing memory port");
    // Reservar el estado y copiar los buffers antes de aceptar. El emisor
    // puede reutilizar sus objetos al volver de esta llamada.
    active = std::make_unique<TransactionState>();
    active->request = request;
    active->context = context;
    active->data = request.direction == MemoryDirection::Store
                       ? request.storeData
                       : ByteBuffer(request.size, 0);
    makeFragments();
    wakeup();
    return TransferResult::Accepted;
}

void
VectorMemoryBackend::makeFragments()
{
    const auto &logical = active->request;
    const Addr line_bytes = active->context->getSystemPtr()->cacheLineSize();
    const Addr page_bytes =
        active->context->getProcessPtr()->pTable->pageSize();
    fatal_if(!line_bytes || (line_bytes & (line_bytes - 1)) || !page_bytes ||
                 (page_bytes & (page_bytes - 1)),
             "Vector memory requires power-of-two cache line and page sizes");

    // Crear todos los fragmentos antes de publicar callbacks a la MMU.
    // offset avanza exactamente los bytes cubiertos: no hay huecos ni
    // solapamientos y la identidad lógica original se mantiene intacta.
    uint32_t offset = 0;
    while (offset < logical.size) {
        const Addr address = (logical.virtualAddress + offset) & addressMask;
        // Cortar por la frontera más cercana permite traducir cada página
        // por separado y evita que un paquete atraviese una línea de caché.
        const Addr size = std::min({Addr{logical.size - offset},
                                    line_bytes - address % line_bytes,
                                    page_bytes - address % page_bytes});
        auto request = std::make_shared<Request>(
            address, size, Request::Flags(0), logical.requestorId, logical.pc,
            logical.taskKey.command.contextId);
        active->fragments.push_back(std::make_unique<FragmentState>(
            *this, active->fragments.size(), offset, std::move(request)));
        offset += size;
    }
}

BaseMMU::Mode
VectorMemoryBackend::translationMode() const
{
    return active->request.direction == MemoryDirection::Load ? BaseMMU::Read
                                                              : BaseMMU::Write;
}

bool
VectorMemoryBackend::hasReadyWork() const
{
    // Dormir mientras se espera a la MMU o al puerto. En particular, WaitRetry
    // sólo se desbloquea con recvReqRetry; no se sondea memoria cada ciclo.
    if (!active || active->pendingTranslations) {
        return false;
    }
    if (active->fault || active->nextTranslation < active->fragments.size() ||
        active->nextFragment == active->fragments.size()) {
        return true;
    }
    return active->fragments[active->nextFragment]->phase == Phase::Ready;
}

void
VectorMemoryBackend::wakeup()
{
    if (hasReadyWork() && !progressEvent.scheduled()) {
        owner.schedule(progressEvent, owner.clockEdge(Cycles(1)));
    }
}

void
VectorMemoryBackend::evaluate()
{
    if (!hasReadyWork()) {
        return;
    }
    if (active->fault) {
        finish();
        return;
    }
    // Traducir todos los fragmentos antes del primer envío. Así un fallo de
    // traducción en el segundo fragmento de un store no deja escrito el
    // primero. Las cargas siguen el mismo recorrido para simplificar estado.
    if (active->nextTranslation < active->fragments.size()) {
        auto &fragment = *active->fragments[active->nextTranslation];
        panic_if(fragment.phase != Phase::PendingTranslation,
                 "Vector memory fragment translated more than once");
        // La traducción puede terminar dentro de translateTiming: registrar
        // fase y contador antes de llamar, sin sobrescribirlos al retornar.
        fragment.phase = Phase::Translating;
        ++active->pendingTranslations;
        active->context->getMMUPtr()->translateTiming(
            fragment.request, active->context, &fragment, translationMode());
        // finishTranslation programa progreso incluso si responde al instante.
        return;
    }
    if (active->nextFragment == active->fragments.size()) {
        finish();
        return;
    }
    auto &fragment = *active->fragments[active->nextFragment];
    panic_if(fragment.phase != Phase::Ready,
             "Vector memory fragment is not ready to send");
    // El índice sólo avanza al recibir la respuesta; nunca hay dos paquetes
    // físicos esperando respuesta simultáneamente.
    sendFragment(fragment);
}

void
VectorMemoryBackend::finishTranslation(FragmentState &fragment,
                                       const Fault &fault,
                                       const RequestPtr &request,
                                       ThreadContext *context,
                                       BaseMMU::Mode mode)
{
    panic_if(!active || fragment.index != active->nextTranslation ||
                 fragment.index >= active->fragments.size() ||
                 active->fragments[fragment.index].get() != &fragment ||
                 fragment.phase != Phase::Translating ||
                 active->pendingTranslations != 1 ||
                 fragment.request != request || active->context != context ||
                 mode != translationMode(),
             "Unexpected vector memory translation completion");
    // Este callback registra el resultado. El cierre de la transacción queda
    // para otro evento, cuando la MMU ya no esté ejecutando este método.
    --active->pendingTranslations;
    ++active->nextTranslation;
    if (fault != NoFault) {
        recordFault(fragment, fault);
    } else {
        panic_if(!request->hasPaddr() || request->isLocalAccess(),
                 "SE vector translation must produce a memory address");
        const Addr line_bytes = context->getSystemPtr()->cacheLineSize();
        panic_if(request->getSize() >
                     line_bytes - request->getPaddr() % line_bytes,
                 "Translated vector fragment crosses a physical cache line");
        fragment.phase = Phase::Ready;
    }
    wakeup();
}

void
VectorMemoryBackend::sendFragment(FragmentState &fragment)
{
    if (!fragment.retryPacket) {
        const bool load = active->request.direction == MemoryDirection::Load;
        auto *packet = new Packet(fragment.request,
                                  load ? MemCmd::ReadReq : MemCmd::WriteReq);
        // El paquete posee su buffer. Copiar los bytes evita compartir memoria
        // cuya liberación pudiera confundirse con la del buffer lógico.
        packet->allocate();
        if (!load) {
            std::copy_n(active->data.data() + fragment.offset,
                        fragment.request->getSize(),
                        packet->getPtr<uint8_t>());
        }
        packet->pushSenderState(new ReturnState(active.get(), fragment.index));
        fragment.retryPacket = packet;
    }

    // Habilitar recepción antes de enviar: una respuesta inmediata puede
    // liberar el paquete dentro de sendTimingReq. El fragmento sigue vivo y
    // su fase permite saber si el callback ya completó la operación.
    PacketPtr packet = fragment.retryPacket;
    fragment.retryPacket = nullptr;
    fragment.phase = Phase::Sending;
    if (port.sendTimingReq(packet)) {
        if (fragment.phase == Phase::Sending) {
            fragment.phase = Phase::WaitResponse;
        }
    } else {
        panic_if(fragment.phase != Phase::Sending,
                 "Memory responded to a rejected vector packet");
        // El puerto no adquirió el paquete. Conservar exactamente este objeto
        // y sus datos hasta que recvReqRetry permita un nuevo intento.
        fragment.retryPacket = packet;
        fragment.phase = Phase::WaitRetry;
    }
}

void
VectorMemoryBackend::recvReqRetry()
{
    panic_if(!active || active->nextFragment >= active->fragments.size(),
             "Vector memory retry without a transaction");
    auto &fragment = *active->fragments[active->nextFragment];
    panic_if(fragment.phase != Phase::WaitRetry || !fragment.retryPacket,
             "Vector memory retry without a rejected packet");
    // El aviso autoriza un intento nuevo, no un sondeo periódico. Si vuelve
    // a fallar el envío, se esperará otro aviso con el mismo paquete.
    fragment.phase = Phase::Ready;
    wakeup();
}

bool
VectorMemoryBackend::recvTimingResp(PacketPtr packet)
{
    panic_if(!active || !packet || !packet->isResponse(),
             "Unexpected vector memory packet response");
    // Recuperar la asociación explícita; no inferirla del orden de llegada.
    // La fase y el RequestPtr también deben coincidir con el acceso pendiente.
    auto *state = dynamic_cast<ReturnState *>(packet->senderState);
    panic_if(!state || state->transaction != active.get() ||
                 state->fragment != active->nextFragment ||
                 state->fragment >= active->fragments.size(),
             "Vector memory response has invalid correlation state");
    auto &fragment = *active->fragments[state->fragment];
    panic_if((fragment.phase != Phase::Sending &&
              fragment.phase != Phase::WaitResponse) ||
                 packet->req != fragment.request ||
                 packet->getSize() != fragment.request->getSize(),
             "Duplicate or mismatched vector memory packet response");

    if (packet->isError()) {
        // Un error del puerto se convierte en un fault del objetivo. Los de
        // traducción ya llegan como Fault directamente desde la MMU.
        const Fault fault = accessFault(fragment.request->getVaddr(),
                                        active->request.direction);
        panic_if(fault == NoFault, "Access-fault factory returned NoFault");
        recordFault(fragment, fault);
    } else {
        const bool load = active->request.direction == MemoryDirection::Load;
        panic_if(load ? !packet->isRead() : !packet->isWrite(),
                 "Vector memory response has the wrong direction");
        if (load) {
            panic_if(!packet->hasData(), "Vector load response has no data");
            // Reunir bytes en su posición original, sin publicar todavía una
            // carga parcial ni reinterpretar su orden según el host.
            std::copy_n(packet->getConstPtr<uint8_t>(), packet->getSize(),
                        active->data.data() + fragment.offset);
        }
        fragment.phase = Phase::Complete;
        ++active->nextFragment;
    }
    // La respuesta devuelve la propiedad del paquete. Retirar nuestro estado
    // y liberarlo una sola vez; RequestPtr conserva su gestión compartida.
    delete packet->popSenderState();
    delete packet;
    wakeup();
    // La recepción estaba reservada al aceptar la petición lógica: no hay
    // backpressure de respuestas ni necesidad de que memoria las reenvíe.
    return true;
}

void
VectorMemoryBackend::recordFault(FragmentState &fragment, const Fault &fault)
{
    panic_if(fault == NoFault, "Cannot record an empty vector memory fault");
    fragment.fault = fault;
    fragment.phase = Phase::Failed;
    if (!active->fault) {
        // Conservar dirección del fragmento e índice del elemento original.
        // Un error de acceso no revierte stores ya confirmados anteriormente.
        active->fault = FaultInfo{fault, fragment.request->getVaddr(),
                                  active->request.elementIndex};
    }
}

void
VectorMemoryBackend::finish()
{
    // Sólo una respuesta terminal por petición. Antes de emitirla no puede
    // quedar ningún objeto que memoria o la MMU todavía puedan referenciar.
    panic_if(!active || active->pendingTranslations ||
                 (!active->fault &&
                  active->nextFragment != active->fragments.size()),
             "Cannot complete unfinished vector memory work");
    for (const auto &fragment : active->fragments) {
        panic_if(fragment->retryPacket ||
                     fragment->phase == Phase::Translating ||
                     fragment->phase == Phase::Sending ||
                     fragment->phase == Phase::WaitResponse ||
                     fragment->phase == Phase::WaitRetry,
                 "Vector memory completion still has outstanding work");
    }

    VectorMemoryResponse response;
    response.taskKey = active->request.taskKey;
    response.requestId = active->request.requestId;
    if (active->fault) {
        response.status = MemoryResponseStatus::Fault;
        response.fault = active->fault;
    } else if (active->request.direction == MemoryDirection::Load) {
        response.status = MemoryResponseStatus::LoadData;
        response.data = active->data;
    } else {
        response.status = MemoryResponseStatus::StoreAck;
    }

    // Liberar capacidad antes del callback permite una nueva petición en él.
    // completed conserva la transacción anterior hasta que el callback vuelva;
    // la respuesta lleva su propia copia de los datos y del fault.
    auto completed = std::move(active);
    sendResponse(response);
}

} // namespace gem5::vector_engine
