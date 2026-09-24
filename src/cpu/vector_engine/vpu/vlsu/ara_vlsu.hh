// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__
#define __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__

#include <functional>
#include <optional>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/memory_types.hh"
#include "cpu/vector_engine/common/unit_completion.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
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
 * Los receptores copian los mensajes aceptados y pueden responder dentro de
 * la propia llamada de envío. Ningún callback puede destruir esta VLSU;
 * tanto el propietario como el mapper deben sobrevivir a ella.
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
    using ReadSender =
        std::function<TransferResult(LaneId, const VrfReadRequest &)>;
    using WriteSender =
        std::function<TransferResult(LaneId, const VrfWriteRequest &)>;
    using CompletionSender = std::function<void(const UnitCompletion &)>;

    AraVLSU(ClockedObject &owner, const AddressMapper &mapper,
            unsigned address_bits, MemorySender send_memory,
            ReadSender send_read, WriteSender send_write,
            CompletionSender send_completion);
    ~AraVLSU();

    AraVLSU(const AraVLSU &) = delete;
    AraVLSU &operator=(const AraVLSU &) = delete;

    // Copia la tarea si hay capacidad; Accepted no indica finalización.
    TransferResult acceptTask(const MemoryTask &task);

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
    // SEND intenta entregar el mensaje; WAIT espera su única respuesta.
    // Carga: Prepare -> SendMemory -> SendWrite -> NextElement.
    // Store: Prepare -> SendRead -> SendMemory -> NextElement.
    // Cada envío aceptado pasa por su WAIT antes de habilitar el siguiente.
    enum class Phase
    {
        Prepare,
        SendRead,
        WaitRead,
        SendMemory,
        WaitMemory,
        SendWrite,
        WaitWrite,
        NextElement,
        MemoryFault,
    };

    struct ElementState
    {
        // Mantiene (TaskKey, requestId) hasta cerrar también el acceso al VRF.
        VectorMemoryRequest request;
        // Identidad independiente de memoria, asociada al mismo elemento.
        VrfAccess vrfAccess;
        // Conserva los bytes de carga mientras el LRF rechaza o confirma
        // su escritura. En stores los bytes están en request.storeData.
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

    ClockedObject &owner;
    const AddressMapper &mapper;
    Addr addressMask;
    MemorySender sendMemory;
    ReadSender sendRead;
    WriteSender sendWrite;
    CompletionSender sendCompletion;
    std::optional<TaskState> active;
    // Un solo evento evita duplicar reintentos y no depende de actividad CPU.
    EventFunctionWrapper progressEvent;

    void validateTask(const MemoryTask &task) const;
    void prepareElement();
    bool hasReadyWork() const;
    void evaluate();
    void handleTransfer(TransferResult result, Phase send, Phase wait);
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_VLSU_ARA_VLSU_HH__
