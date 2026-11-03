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
 * Fronteras de memoria del contexto que limitan un bloque unit-stride.
 * El backend las obtiene del sistema y del proceso SE; la VLSU sólo recibe
 * estos valores y no accede a ThreadContext, MMU ni objetos de gem5.
 * Ambos tamaños son bytes y potencias de dos.
 */
struct MemoryBoundaries
{
    Addr lineBytes = 0;
    Addr pageBytes = 0;

    bool
    valid() const
    {
        return lineBytes && !(lineBytes & (lineBytes - 1)) && pageBytes &&
               !(pageBytes & (pageBytes - 1));
    }
};

/**
 * Bloque de elementos consecutivos de una carga o store unit-stride.
 *
 * Un bloque de varios elementos no cruza una línea de caché, una página ni
 * el final del espacio de direcciones del objetivo, de modo que el backend
 * lo traduce una vez y lo transporta en un solo paquete físico. Un elemento
 * que no cabe entero antes de una de esas fronteras viaja en un bloque de un
 * elemento, y el backend lo divide en los fragmentos físicos necesarios.
 * En ambos casos el backend devuelve una única respuesta con esta identidad.
 *
 * Fault: un bloque de varios elementos está en una sola página y en un solo
 * paquete, así que un fallo de traducción o de acceso afecta a todos sus
 * bytes y ninguno se ha escrito. El índice del fault es firstElement, que
 * pertenece a la parte que falla. Los bloques anteriores ya están cerrados.
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
    // Índices del vector original; size = elementCount * elementBytes.
    // La lane que contiene los datos se obtiene por separado con el mapper.
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
     * Comprueba que el número de elementos coincide con los bytes pedidos
     * y con su posición en el grupo de registros. No comprueba la dirección
     * de memoria ni el contenido de los buffers; el backend comprueba
     * además que un bloque de varios elementos no cruce sus fronteras.
     */
    bool
    validBlockShape() const
    {
        // Multiplicar en 64 bits evita desbordar antes de comparar con size.
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
 * LoadData lleva los bytes del bloque y ningún fault; StoreAck sin datos ni
 * fault. Fault sólo lleva causa, dirección virtual del paquete o fragmento
 * que falla e índice firstElement del bloque.
 * La respuesta de carga aún debe repartirse en writebacks al VRF.
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
 * AraVLSU conserva aquí los datos y la identidad de memoria hasta recibir
 * todos los WriteAck de las palabras locales en que se reparte el bloque.
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
