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
    uint64_t vl = 0;
    uint64_t vstart = 0;
    uint16_t sewBits = 0;
    VectorLmul lmul = VectorLmul::Invalid;
    bool masked = false;
    bool tailAgnostic = false;
    bool maskAgnostic = false;
    bool illegalVtype = false;
};

constexpr bool
operator==(const VectorConfig &lhs, const VectorConfig &rhs)
{
    return lhs.vl == rhs.vl && lhs.vstart == rhs.vstart &&
           lhs.sewBits == rhs.sewBits && lhs.lmul == rhs.lmul &&
           lhs.masked == rhs.masked && lhs.tailAgnostic == rhs.tailAgnostic &&
           lhs.maskAgnostic == rhs.maskAgnostic &&
           lhs.illegalVtype == rhs.illegalVtype;
}

constexpr bool
operator!=(const VectorConfig &lhs, const VectorConfig &rhs)
{
    return !(lhs == rhs);
}

/**
 * Operación que MinorCPU describe y TaskDistributor entrega a las lanes.
 * Ninguna unidad necesita volver a decodificar instrucciones.
 *
 * Todas siguen la forma RVV vd[i] = vs2[i] op (vs1[i] | rs1): el primer
 * operando (ArithmeticCommand::vectorSource) es vs2 y el segundo es vs1 o
 * el escalar rs1. El orden importa en Divide, Remainder y FloatDivide.
 *
 * - Add, Multiply: enteros; Multiply conserva los SEW bits bajos.
 * - Divide, Remainder: enteros; el signo viaja en ElementSignedness.
 * - FloatAdd: binary16 interno y binary32 desde CPU.
 * - FloatMultiply, FloatDivide: binary32.
 *
 * La ficha de OperationTraits decide la unidad y los anchos admitidos;
 * que un valor exista aquí no implica que la CPU lo genere.
 */
enum class ArithmeticOperation : uint8_t
{
    Invalid,
    Add,
    FloatAdd,
    Multiply,
    Divide,
    Remainder,
    FloatMultiply,
    FloatDivide,
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
 * Divide y Remainder exigen Signed (vdiv, vrem) o Unsigned (vdivu, vremu);
 * las demás operaciones exigen NotApplicable, porque sus bits de resultado
 * no dependen del signo. OperationTraits declara qué caso aplica.
 */
enum class ElementSignedness : uint8_t
{
    NotApplicable,
    Signed,
    Unsigned,
};

/**
 * Slide de un elemento que MinorCPU describe y SlideUnit ejecuta, según
 * RVV 1.0 (edición 20240411, Vector Slide1up y Vector Slide1down):
 *
 * - Slide1Up: vd[0] = x[rs1] y vd[i] = vs2[i-1] para los demás índices.
 * - Slide1Down: vd[vl-1] = x[rs1] y vd[i] = vs2[i+1] para los demás.
 *
 * El escalar sólo se inserta si su índice de destino está activo. Ambas
 * mueven patrones de bits del ancho SEW; no interpretan signo ni formato.
 */
enum class SlideOperation : uint8_t
{
    Invalid,
    Slide1Up,
    Slide1Down,
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
 * Lanes corresponde al reparto aritmético; Vlsu, a las operaciones de
 * memoria; Slide, a los movimientos entre elementos de SlideUnit.
 */
enum class VectorUnitClass : uint8_t
{
    Invalid,
    Lanes,
    Vlsu,
    Slide,
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VECTOR_TYPES_HH__
