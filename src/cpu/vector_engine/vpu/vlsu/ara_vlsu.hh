// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__
#define __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__

#include <functional>
#include <optional>
#include <vector>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/memory_types.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/common/unit_completion.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "cpu/vector_engine/vpu/register_file/vrf_port.hh"
#include "cpu/vector_engine/vpu/vlsu/memory_block_planner.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Ejecuta una tarea unit-stride por bloques de elementos contiguos, con un
 * único bloque activo. Cada bloque es una petición lógica de memoria y se
 * reparte en accesos al VRF por palabra local según el mapper:
 *
 * - Store: lee del VRF todas las palabras del bloque, reúne sus bytes en el
 *   buffer por offset, envía el bloque y espera StoreAck.
 * - Carga: envía el bloque, recibe todos sus bytes y escribe cada palabra
 *   en su LRF; el bloque termina al recibir todos los WriteAck.
 *
 * El siguiente bloque no prepara accesos hasta cerrar memoria y VRF del
 * actual. Los bloques se forman con planMemoryBlock() a partir de las
 * fronteras de línea y página que proporciona el backend.
 *
 * El propietario conecta los callbacks al backend, los LRF y el sequencer.
 * El backend copia los mensajes aceptados y puede responder dentro de la
 * llamada de envío. Cada LRF consulta vrfInputsPerLane entradas
 * persistentes y notifica cada concesión antes de devolver datos o WriteAck.
 * Ningún callback puede destruir esta VLSU; tanto el propietario como el
 * mapper deben sobrevivir a ella.
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
    using BoundaryQuery = std::function<MemoryBoundaries(ContextID)>;
    using VrfWakeup = std::function<void(LaneId)>;
    using CompletionSender = std::function<void(const UnitCompletion &)>;

    AraVLSU(ClockedObject &owner, const AddressMapper &mapper,
            const VlsuParameters &parameters, unsigned address_bits,
            MemorySender send_memory, BoundaryQuery query_boundaries,
            VrfWakeup wake_vrf, CompletionSender send_completion);
    ~AraVLSU();

    AraVLSU(const AraVLSU &) = delete;
    AraVLSU &operator=(const AraVLSU &) = delete;

    // Copia la tarea si hay capacidad; Accepted no indica finalización.
    TransferResult acceptTask(const MemoryTask &task);

    // Entrada persistente input de la LRF lane_id. Ofrece el input-ésimo
    // acceso publicado y no concedido del bloque activo en esa lane.
    VrfRequestPort vrfPort(LaneId lane_id, uint32_t input);

    // Las respuestas actualizan el estado y programan progreso posterior.
    // No ejecutan recursivamente el siguiente bloque dentro del callback.
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
    // Fases del bloque activo. PUBLISH crea las ofertas al VRF; GATHER y
    // SCATTER esperan todas sus respuestas. WAIT_MEMORY espera la única
    // respuesta terminal del backend. Sólo las fases sin operación aceptada
    // pendiente programan progreso (véase hasReadyWork()).
    enum class Phase
    {
        Prepare,
        PublishReads,
        GatherReads,
        SendMemory,
        WaitMemory,
        PublishWrites,
        ScatterWrites,
        NextBlock,
        MemoryFault,
    };

    // Estado de un acceso por palabra. Offered: visible para su LRF hasta
    // la concesión. Granted: el LRF debe devolver una respuesta terminal.
    enum class AccessPhase
    {
        Idle,
        Offered,
        Granted,
        Done,
    };

    /**
     * Acceso al VRF de una palabra local del bloque. Su accessId es único en
     * la tarea y distinto del requestId del bloque. bufferOffset sitúa sus
     * bytes en el buffer del bloque; no depende del orden de respuestas.
     */
    struct BlockVrfAccess
    {
        LaneId laneId = 0;
        VrfAccess access;
        uint32_t bufferOffset = 0;
        AccessPhase phase = AccessPhase::Idle;
        // Estable desde la publicación hasta la concesión; se conserva hasta
        // la respuesta para mantener la asociación con su productor.
        std::optional<VrfRequest> request;
        Tick eligibleAt = 0;
        Tick publishedAt = 0;
    };

    struct BlockState
    {
        // Mantiene (TaskKey, requestId) hasta cerrar también el VRF. En
        // stores, request.storeData es el buffer de reunión del bloque.
        VectorMemoryRequest request;
        bool splitElement = false;
        // Accesos en orden creciente de bytes; accessId = firstAccessId + i.
        std::vector<BlockVrfAccess> accesses;
        VrfAccessId firstAccessId = InvalidVrfAccessId;
        // Accesos concedidos o publicados cuya respuesta falta.
        std::size_t pendingAccesses = 0;
        // Conserva los bytes de carga hasta recibir todos los WriteAck.
        std::optional<LoadData> load;
        // Inicio de la fase actual, para separar esperas de VRF y memoria.
        Tick phaseStart = 0;
    };

    struct TaskState
    {
        MemoryTask task;
        MemoryBlockLimits limits;
        uint32_t elementBytes = 0;
        // Índice absoluto dentro del vector, no relativo a vstart.
        uint32_t nextElement = 0;
        // Contadores separados; un retry no consume identificadores nuevos.
        RequestId nextRequestId = 0;
        VrfAccessId nextAccessId = 0;
        Phase phase = Phase::Prepare;
        std::optional<BlockState> block;
        std::optional<FaultInfo> fault;
    };

    ModuleStats stats;
    statistics::Scalar loadTasks;
    statistics::Scalar storeTasks;
    statistics::Scalar loadElements;
    statistics::Scalar storeElements;
    statistics::Scalar loadBytes;
    statistics::Scalar storeBytes;
    statistics::Scalar blocks;
    statistics::Scalar splitElementBlocks;
    statistics::Distribution blockBytes;
    statistics::Distribution blockVrfAccesses;
    statistics::Scalar memoryRequests;
    statistics::Scalar memoryRetries;
    statistics::Scalar memoryTicks;
    statistics::Scalar vrfReadsGranted;
    statistics::Scalar vrfWritesGranted;
    statistics::Scalar vrfGatherTicks;
    statistics::Scalar vrfScatterTicks;
    ClockedObject &owner;
    const AddressMapper &mapper;
    const VlsuParameters &parameters;
    Addr addressMask;
    MemorySender sendMemory;
    BoundaryQuery queryBoundaries;
    VrfWakeup wakeVrf;
    CompletionSender sendCompletion;
    std::optional<TaskState> active;
    // Un solo evento evita duplicar reintentos y no depende de actividad CPU.
    EventFunctionWrapper progressEvent;

    std::optional<VrfOffer> peekOffer(LaneId lane_id, uint32_t input) const;
    BlockVrfAccess &findAccess(const VrfAccessKey &key, AccessPhase phase);
    void recvVrfGrant(const VrfAccessKey &key);
    void validateTask(const MemoryTask &task) const;
    void prepareBlock();
    void publishAccesses(bool write);
    bool hasReadyWork() const;
    void evaluate();
    void handleMemoryTransfer(TransferResult result);
    void closeBlock();
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__
