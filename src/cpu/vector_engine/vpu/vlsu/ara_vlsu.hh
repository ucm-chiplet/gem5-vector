// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__
#define __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__

#include <functional>
#include <optional>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/memory_types.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/common/unit_completion.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "cpu/vector_engine/vpu/register_file/vrf_port.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Ejecuta una tarea y un elemento completo cada vez, incluido su acceso al
 * VRF. Una carga termina tras WriteAck; un store, tras StoreAck. Recibir los
 * datos de memoria todavía no permite dar por terminada una carga.
 *
 * El propietario conecta los callbacks al backend, los LRF y el sequencer.
 * El backend copia los mensajes aceptados y puede responder dentro de la
 * llamada de envío. El VRF consulta una petición persistente y notifica su
 * concesión antes de devolver datos o WriteAck. Ningún callback puede destruir
 * esta VLSU; tanto el propietario como el mapper deben sobrevivir a ella.
 *
 * address_bits indica el ancho de dirección del objetivo (32 o 64), igual
 * al configurado en el backend. Admisión comprueba los LMUL soportados; aquí
 * se valida la coherencia de la tarea con el mapper y la capacidad efectiva.
 * El propietario coordina drain: el trabajo admitido termina y sólo se
 * destruye el módulo cuando isIdle(). No hay reset ni cancelación activa.
 */
class AraVLSU
{
  public:
    using MemorySender =
        std::function<TransferResult(const VectorMemoryRequest &)>;
    using VrfWakeup = std::function<void(LaneId)>;
    using CompletionSender = std::function<void(const UnitCompletion &)>;

    AraVLSU(ClockedObject &owner, const AddressMapper &mapper,
            const VlsuParameters &parameters, unsigned address_bits,
            MemorySender send_memory, VrfWakeup wake_vrf,
            CompletionSender send_completion);
    ~AraVLSU();

    AraVLSU(const AraVLSU &) = delete;
    AraVLSU &operator=(const AraVLSU &) = delete;

    // Copia la tarea si hay capacidad; Accepted no indica finalización.
    TransferResult acceptTask(const MemoryTask &task);

    // One persistent input per LRF. Only the active element's LRF sees it.
    VrfRequestPort vrfPort(LaneId lane_id);

    // Las respuestas actualizan el estado y programan progreso posterior.
    // No ejecutan recursivamente el siguiente elemento dentro del callback.
    void recvMemoryResponse(const VectorMemoryResponse &response);
    void recvVrfReadResponse(const ReadResponse &response);
    void recvVrfWriteAck(const WriteAck &ack);
    void wakeup();

    // Estado local: el propietario debe consultar también al resto de la VPU.
    bool
    isIdle() const
    {
        return !active && !progressEvent.scheduled();
    }

  private:
    // SEND prepara el mensaje; OFFER conserva una solicitud VRF hasta grant.
    // WAIT espera la única respuesta de una operación ya concedida/aceptada.
    // La memoria conserva su protocolo Accepted/Retry y un elemento activo.
    enum class Phase
    {
        Prepare,
        SendRead,
        OfferRead,
        WaitRead,
        SendMemory,
        WaitMemory,
        SendWrite,
        OfferWrite,
        WaitWrite,
        NextElement,
        MemoryFault,
    };

    struct ElementState
    {
        // Mantiene (TaskKey, requestId) hasta cerrar también el acceso al VRF.
        VectorMemoryRequest request;
        // Physical placement belongs to local execution, not the block key.
        LaneId laneId = 0;
        // Identidad independiente de memoria, asociada al mismo elemento.
        VrfAccess vrfAccess;
        // Stable until the grant; retained until the response for correlation.
        std::optional<VrfRequest> vrfRequest;
        Tick vrfEligibleAt = 0;
        Tick vrfPublishedAt = 0;
        // Conserva los bytes de carga hasta concesión y confirmación de su
        // escritura. En stores los bytes están en request.storeData.
        std::optional<LoadData> load;
    };

    struct TaskState
    {
        MemoryTask task;
        // Índice absoluto dentro del vector, no relativo a vstart.
        uint32_t nextElement = 0;
        // Contadores separados; un retry no consume identificadores nuevos.
        RequestId nextRequestId = 0;
        VrfAccessId nextAccessId = 0;
        Phase phase = Phase::Prepare;
        std::optional<ElementState> element;
        std::optional<FaultInfo> fault;
    };

    ModuleStats stats;
    statistics::Scalar loadTasks;
    statistics::Scalar storeTasks;
    statistics::Scalar loadElements;
    statistics::Scalar storeElements;
    statistics::Scalar loadBytes;
    statistics::Scalar storeBytes;
    statistics::Scalar memoryRequests;
    statistics::Scalar memoryRetries;
    statistics::Scalar vrfReadsGranted;
    statistics::Scalar vrfWritesGranted;
    ClockedObject &owner;
    const AddressMapper &mapper;
    const VlsuParameters &parameters;
    Addr addressMask;
    MemorySender sendMemory;
    VrfWakeup wakeVrf;
    CompletionSender sendCompletion;
    std::optional<TaskState> active;
    // Un solo evento evita duplicar reintentos y no depende de actividad CPU.
    EventFunctionWrapper progressEvent;

    void recvVrfGrant(const VrfAccessKey &key);
    void validateTask(const MemoryTask &task) const;
    void prepareElement();
    bool hasReadyWork() const;
    void evaluate();
    void handleTransfer(TransferResult result, Phase send, Phase wait);
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__
