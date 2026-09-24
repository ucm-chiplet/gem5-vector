// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_VLSU_VECTOR_MEMORY_BACKEND_HH__
#define __CPU_VECTOR_ENGINE_VPU_VLSU_VECTOR_MEMORY_BACKEND_HH__

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "arch/generic/mmu.hh"
#include "cpu/vector_engine/common/memory_types.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Adapta una petición lógica de la VLSU a la memoria timing en modo SE.
 * Conserva una transacción y, como máximo, un paquete físico en vuelo.
 * El propietario conecta getPort(), resuelve ContextID a ThreadContext y
 * proporciona la función que construye los faults de acceso del objetivo.
 * Los faults de traducción proceden directamente de la MMU del contexto.
 * En RISC-V SE esa MMU admite accesos desalineados; el backend los divide
 * cuando cruzan los límites de página o de línea de caché.
 *
 * El propietario debe ordenar el offload respecto a la memoria escalar y
 * mantener vivos contexto, MMU y backend hasta isIdle(). Drain no cancela
 * traducciones ni paquetes aceptados. El ancho de dirección debe coincidir
 * con el de AraVLSU. Los callbacks pueden responder inmediatamente, pero no
 * destruir este adaptador.
 */
class VectorMemoryBackend
{
  public:
    // Estas dependencias mantienen ThreadContext y los detalles del objetivo
    // dentro del backend; no cruzan la interfaz lógica hacia la VLSU.
    using ContextResolver = std::function<ThreadContext *(ContextID)>;
    using AccessFaultFactory = std::function<Fault(Addr, MemoryDirection)>;
    using ResponseSender = std::function<void(const VectorMemoryResponse &)>;

    VectorMemoryBackend(ClockedObject &owner, unsigned address_bits,
                        ContextResolver resolve_context,
                        AccessFaultFactory access_fault,
                        ResponseSender send_response);
    ~VectorMemoryBackend();

    VectorMemoryBackend(const VectorMemoryBackend &) = delete;
    VectorMemoryBackend &operator=(const VectorMemoryBackend &) = delete;

    // Accepted copia petición y datos; obliga a emitir una respuesta terminal.
    // Si hay una transacción activa, Retry deja la petición en manos de VLSU.
    TransferResult acceptRequest(const VectorMemoryRequest &request);
    RequestPort &
    getPort()
    {
        return port;
    }
    void wakeup();

    // Incluye traducciones, paquetes y respuesta lógica todavía pendiente.
    bool
    isIdle() const
    {
        return !active && !progressEvent.scheduled();
    }

  private:
    // El puerto sólo reenvía notificaciones de gem5 al estado del backend.
    class MemoryPort : public RequestPort
    {
      public:
        MemoryPort(VectorMemoryBackend &backend, const std::string &name);

      protected:
        bool recvTimingResp(PacketPtr packet) override;
        void recvReqRetry() override;

      private:
        VectorMemoryBackend &backend;
    };

    // Estados de un fragmento físico, distintos de las fases de AraVLSU.
    // WaitRetry conserva un paquete rechazado; WaitResponse espera uno que
    // memoria ya aceptó. Sólo el primero permite volver a enviarlo.
    enum class Phase
    {
        PendingTranslation,
        Translating,
        Ready,
        Sending,
        WaitRetry,
        WaitResponse,
        Complete,
        Failed,
    };

    /**
     * Estado y callback de traducción con dirección estable. La transacción
     * lo posee mediante unique_ptr y no se libera dentro de su finish():
     * el evento posterior deja que antes retorne la llamada de la MMU.
     */
    struct FragmentState : public BaseMMU::Translation
    {
        VectorMemoryBackend &backend;
        const std::size_t index;
        // Posición de este fragmento dentro del buffer lógico de cuatro bytes.
        const uint32_t offset;
        // La dirección física sólo es válida tras una traducción correcta.
        RequestPtr request;
        Phase phase = Phase::PendingTranslation;
        // Propiedad local mientras el puerto no lo acepte; nullptr en vuelo.
        PacketPtr retryPacket = nullptr;
        Fault fault = NoFault;

        FragmentState(VectorMemoryBackend &backend, std::size_t index,
                      uint32_t offset, RequestPtr request);
        void markDelayed() override;
        void finish(const Fault &fault, const RequestPtr &request,
                    ThreadContext *context, BaseMMU::Mode mode) override;
    };

    struct TransactionState
    {
        // La clave lógica no cambia al dividir el acceso en fragmentos.
        VectorMemoryRequest request;
        ThreadContext *context = nullptr;
        // Reúne la carga completa o conserva los bytes originales del store.
        ByteBuffer data;
        std::vector<std::unique_ptr<FragmentState>> fragments;
        // Primero se traducen todos; después se envían de uno en uno.
        std::size_t nextTranslation = 0;
        // Impide cerrar la transacción mientras la MMU pueda responder.
        std::size_t pendingTranslations = 0;
        std::size_t nextFragment = 0;
        // Conserva el primer fallo e impide emitir fragmentos posteriores.
        std::optional<FaultInfo> fault;
    };

    // Viaja en el paquete para recuperar transacción y fragmento al volver.
    // No apunta a una instrucción de CPU ni crea otro requestId lógico.
    struct ReturnState : public Packet::SenderState
    {
        TransactionState *transaction;
        std::size_t fragment;

        ReturnState(TransactionState *transaction, std::size_t fragment)
            : transaction(transaction), fragment(fragment)
        {}
    };

    ClockedObject &owner;
    Addr addressMask;
    ContextResolver resolveContext;
    AccessFaultFactory accessFault;
    ResponseSender sendResponse;
    MemoryPort port;
    std::unique_ptr<TransactionState> active;
    EventFunctionWrapper progressEvent;

    void validateRequest(const VectorMemoryRequest &request) const;
    void makeFragments();
    BaseMMU::Mode translationMode() const;
    bool hasReadyWork() const;
    void evaluate();
    void finishTranslation(FragmentState &fragment, const Fault &fault,
                           const RequestPtr &request, ThreadContext *context,
                           BaseMMU::Mode mode);
    void sendFragment(FragmentState &fragment);
    bool recvTimingResp(PacketPtr packet);
    void recvReqRetry();
    void recordFault(FragmentState &fragment, const Fault &fault);
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_VLSU_VECTOR_MEMORY_BACKEND_HH__
