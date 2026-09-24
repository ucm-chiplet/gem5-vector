#ifndef __CPU_VECTOR_ENGINE_COMMON_VECTOR_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_VECTOR_TYPES_HH__

#include <cstdint>
#include <vector>

namespace gem5::vector_engine
{

// Bytes en orden creciente de dirección o desplazamiento dentro del grupo.
using ByteBuffer = std::vector<uint8_t>;

/**
 * Respuesta inmediata a un envío interno. Accepted transfiere una copia
 * del mensaje; Retry no causa efectos ni cambia su identidad.
 */
enum class TransferResult : uint8_t
{
    Accepted,
    Retry,
};

/**
 * Factor LMUL expresado como exponente de dos.
 * MinorCPU lo captura; admisión y las unidades dimensionan con él los grupos.
 * Que un valor exista en el enum no implica que la VPU lo admita.
 */
enum class VectorLmul : int8_t
{
    Mf8 = -3,
    Mf4 = -2,
    Mf2 = -1,
    M1 = 0,
    M2 = 1,
    M4 = 2,
    M8 = 3,
    Invalid = 127,
};

constexpr bool
isValid(VectorLmul lmul)
{
    return lmul >= VectorLmul::Mf8 && lmul <= VectorLmul::M8;
}

/**
 * Configuración RVV que MinorCPU captura una vez al construir el comando.
 * Admisión comprueba su soporte; AraSequencer y las unidades usan esta copia
 * sin volver a consultar los registros de estado de la CPU.
 */
struct VectorConfig
{
    uint32_t vl = 0;
    uint32_t vstart = 0;
    uint16_t sewBits = 0;
    VectorLmul lmul = VectorLmul::Invalid;
    bool masked = false;
    bool tailAgnostic = false;
    bool maskAgnostic = false;
};

constexpr bool
operator==(const VectorConfig &lhs, const VectorConfig &rhs)
{
    return lhs.vl == rhs.vl && lhs.vstart == rhs.vstart &&
           lhs.sewBits == rhs.sewBits && lhs.lmul == rhs.lmul &&
           lhs.masked == rhs.masked &&
           lhs.tailAgnostic == rhs.tailAgnostic &&
           lhs.maskAgnostic == rhs.maskAgnostic;
}

constexpr bool
operator!=(const VectorConfig &lhs, const VectorConfig &rhs)
{
    return !(lhs == rhs);
}

/**
 * Operación que MinorCPU describe y TaskDistributor entrega a las lanes.
 * La ALU usa Add en la versión inicial; no necesita volver a decodificar.
 */
enum class ArithmeticOperation : uint8_t
{
    Invalid,
    Add,
};

/**
 * Relación entre anchos de operandos que MinorCPU incluye en el comando.
 * Admisión sólo permite SameWidth en la versión inicial.
 */
enum class ElementWidthMode : uint8_t
{
    SameWidth,
    Widening,
    Narrowing,
};

/**
 * Interpretación del signo que MinorCPU indica en el comando aritmético.
 * Admisión exige NotApplicable para la suma de la versión inicial.
 */
enum class ElementSignedness : uint8_t
{
    NotApplicable,
    Signed,
    Unsigned,
};

/** MinorCPU indica carga o store; AraVLSU selecciona el recorrido de datos. */
enum class MemoryDirection : uint8_t
{
    Invalid,
    Load,
    Store,
};

/**
 * Orden descrito para accesos indexados; lo transporta IndexedAddress.
 * Admisión rechaza esos accesos en la versión inicial.
 */
enum class MemoryOrdering : uint8_t
{
    NotApplicable,
    Ordered,
    Unordered,
};

/**
 * AraSequencer usa este valor para indicar la unidad responsable de la tarea.
 * Lanes corresponde al reparto aritmético; Vlsu, a las operaciones de memoria.
 */
enum class VectorUnitClass : uint8_t
{
    Invalid,
    Lanes,
    Vlsu,
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VECTOR_TYPES_HH__
