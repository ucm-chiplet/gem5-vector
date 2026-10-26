// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_MEMORY_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_MEMORY_TYPES_HH__

#include <cstdint>
#include <limits>
#include <optional>

#include "cpu/vector_engine/common/vrf_types.hh"
#include "cpu/vector_engine/interface/vector_completion.hh"
#include "mem/request.hh"

namespace gem5::vector_engine
{

// AraVLSU asigna IDs crecientes por TaskKey y los conserva en cada retry.
// El valor reservado nunca identifica una petición emitida.
using RequestId = uint32_t;
inline constexpr RequestId InvalidRequestId =
    std::numeric_limits<RequestId>::max();

/**
 * Logical contiguous element block. Phase 1 admits elementCount == 1 only.
 * El backend puede dividirla en
 * fragmentos físicos, pero devuelve una sola respuesta con la misma clave.
 * No transporta objetos de instrucción CPU ni paquetes de gem5.
 */
struct VectorMemoryRequest
{
    // La pareja incluye CommandKey, contexto, taskId y requestId.
    TaskKey taskKey;
    RequestId requestId = InvalidRequestId;
    MemoryDirection direction = MemoryDirection::Invalid;
    // Destino de carga o fuente de store; dataRange es relativo al grupo.
    VectorRegRef registerRef;
    ByteRange dataRange;
    // Absolute elements; size = elementCount * elementBytes. No unique lane.
    ElementRange elements;
    uint32_t elementBytes = 0;
    Addr virtualAddress = 0;
    uint32_t size = 0;
    // Vacío en carga; size bytes ya capturados del VRF en store.
    ByteBuffer storeData;
    // size entradas a uno; no es la máscara de predicación.
    ByteEnable byteEnable;
    // Metadatos originales de CPU; contextId viaja dentro de taskKey.
    Addr pc = 0;
    RequestorID requestorId = Request::invldRequestorId;

    /**
     * Comprueba en 64 bits las relaciones elementos/bytes del descriptor.
     * No valida soporte, direcciones ni buffers y no activa agrupación:
     * el backend actual exige además elementCount == 1.
     */
    bool
    validBlockShape() const
    {
        return elements.valid() && elementBytes && dataRange.valid() &&
               uint64_t{elements.elementCount} * elementBytes == size &&
               dataRange.size == size &&
               uint64_t{elements.firstElement} * elementBytes ==
                   dataRange.offset;
    }
};

enum class MemoryResponseStatus : uint8_t
{
    LoadData,
    StoreAck,
    Fault,
};

/**
 * Única respuesta terminal de una petición aceptada, sin fragmentos en vuelo.
 * LoadData lleva los bytes del elemento y ningún fault; StoreAck sin datos ni
 * fault. Fault sólo lleva causa, dirección virtual e índice del elemento.
 * La respuesta de carga aún debe convertirse en un writeback al VRF.
 */
struct VectorMemoryResponse
{
    TaskKey taskKey;
    RequestId requestId = InvalidRequestId;
    MemoryResponseStatus status = MemoryResponseStatus::Fault;
    ByteBuffer data;
    std::optional<FaultInfo> fault;
};

/**
 * AraVLSU conserva aquí los datos y la identidad de memoria hasta WriteAck.
 * Los metadatos de destino se copian de la petición original conservada;
 * no se deducen de la respuesta ni se envían a la ALU.
 */
struct LoadData
{
    TaskKey taskKey;
    RequestId requestId = InvalidRequestId;
    VectorRegRef destinationReg;
    ElementRange elements;
    uint32_t elementBytes = 0;
    ByteRange destinationByteRange;
    ByteBuffer data;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_MEMORY_TYPES_HH__
