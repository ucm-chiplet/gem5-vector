#ifndef __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMMAND_HH__
#define __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMMAND_HH__

#include <cstdint>
#include <variant>

#include "base/types.hh"
#include "cpu/vector_engine/common/command_key.hh"
#include "cpu/vector_engine/common/vector_reg_ref.hh"
#include "cpu/vector_engine/common/vector_types.hh"
#include "mem/request.hh"

namespace gem5::vector_engine
{

// --------------------------------------------------------------------------
// Tipos de comandos aritméticos.
// --------------------------------------------------------------------------

/**
 * MinorCPU captura aquí el segundo operando que consumirán las lanes.
 * La alternativa distingue registro vectorial, valor escalar o inmediato,
 * sin duplicar esa información en otro campo. El inmediato queda reservado.
 */
using ArithmeticOperand = std::variant<VectorRegRef, RegVal, int64_t>;

/**
 * Operación y operandos que MinorCPU entrega a la VPU ya decodificados.
 * AraSequencer los copia a ArithmeticTask para TaskDistributor y las lanes.
 */
struct ArithmeticCommand
{
    ArithmeticOperation operation = ArithmeticOperation::Invalid;
    ElementWidthMode widthMode = ElementWidthMode::SameWidth;
    ElementSignedness signedness = ElementSignedness::NotApplicable;
    VectorRegRef destination;
    VectorRegRef vectorSource;
    ArithmeticOperand secondOperand;
};

inline bool
operator==(const ArithmeticCommand &lhs, const ArithmeticCommand &rhs)
{
    return lhs.operation == rhs.operation &&
           lhs.widthMode == rhs.widthMode &&
           lhs.signedness == rhs.signedness &&
           lhs.destination == rhs.destination &&
           lhs.vectorSource == rhs.vectorSource &&
           lhs.secondOperand == rhs.secondOperand;
}

inline bool
operator!=(const ArithmeticCommand &lhs, const ArithmeticCommand &rhs)
{
    return !(lhs == rhs);
}

// --------------------------------------------------------------------------
// Formas de direccionamiento de memoria.
// --------------------------------------------------------------------------

/**
 * MinorCPU indica elementos consecutivos en memoria.
 * AraVLSU usa la base y el índice de elemento para obtener cada dirección.
 */
struct UnitStrideAddress
{};

constexpr bool
operator==(const UnitStrideAddress &, const UnitStrideAddress &)
{
    return true;
}

constexpr bool
operator!=(const UnitStrideAddress &, const UnitStrideAddress &)
{
    return false;
}

/**
 * Describe una separación entre elementos para una ampliación futura.
 * Admisión reconoce la variante, pero la rechaza en la versión inicial.
 */
struct StridedAddress
{
    RegVal stride = 0;
};

constexpr bool
operator==(const StridedAddress &lhs, const StridedAddress &rhs)
{
    return lhs.stride == rhs.stride;
}

constexpr bool
operator!=(const StridedAddress &lhs, const StridedAddress &rhs)
{
    return !(lhs == rhs);
}

/**
 * Describe registros de índices y orden para una ampliación futura.
 * Admisión rechaza esta variante; AraVLSU aún no ejecuta accesos indexados.
 */
struct IndexedAddress
{
    VectorRegRef index;
    MemoryOrdering ordering = MemoryOrdering::NotApplicable;
};

constexpr bool
operator==(const IndexedAddress &lhs, const IndexedAddress &rhs)
{
    return lhs.index == rhs.index && lhs.ordering == rhs.ordering;
}

constexpr bool
operator!=(const IndexedAddress &lhs, const IndexedAddress &rhs)
{
    return !(lhs == rhs);
}

/**
 * Forma de direccionamiento que MinorCPU incluye en MemoryCommand.
 * Admisión sólo admite UnitStrideAddress en la versión inicial.
 */
using MemoryAddressing = std::variant<
    UnitStrideAddress,
    StridedAddress,
    IndexedAddress>;

// --------------------------------------------------------------------------
// Comando de memoria.
// --------------------------------------------------------------------------

/**
 * Carga o store descrito por MinorCPU con base escalar ya capturada.
 * AraSequencer lo copia a MemoryTask; AraVLSU usa dataReg como destino de
 * las cargas o fuente de los stores.
 */
struct MemoryCommand
{
    MemoryDirection direction = MemoryDirection::Invalid;
    VectorRegRef dataReg;
    Addr base = 0;
    MemoryAddressing addressing;
    uint16_t elementWidthBits = 0;
    uint8_t fieldCount = 0;
    bool faultOnlyFirst = false;
};

inline bool
operator==(const MemoryCommand &lhs, const MemoryCommand &rhs)
{
    return lhs.direction == rhs.direction &&
           lhs.dataReg == rhs.dataReg && lhs.base == rhs.base &&
           lhs.addressing == rhs.addressing &&
           lhs.elementWidthBits == rhs.elementWidthBits &&
           lhs.fieldCount == rhs.fieldCount &&
           lhs.faultOnlyFirst == rhs.faultOnlyFirst;
}

inline bool
operator!=(const MemoryCommand &lhs, const MemoryCommand &rhs)
{
    return !(lhs == rhs);
}

// --------------------------------------------------------------------------
// Contenido específico del comando.
// --------------------------------------------------------------------------

/**
 * MinorCPU selecciona una operación aritmética o de memoria.
 * AraSequencer usa la alternativa para crear la tarea de la unidad adecuada.
 */
using VectorCommandPayload =
    std::variant<ArithmeticCommand, MemoryCommand>;

// --------------------------------------------------------------------------
// Descriptor compartido entre CPU y VPU.
// --------------------------------------------------------------------------

/**
 * Comando que MinorCPU construye con semántica y operandos ya capturados.
 * CpuVectorInterface lo entrega a admisión; CommandQueue lo conserva y
 * AraSequencer lo transforma en trabajo para las unidades.
 * Se mantiene inmutable durante los reintentos. La VPU no recupera una
 * StaticInst o DynInst a partir de este descriptor ni vuelve a decodificar.
 */
struct VectorCommand
{
    CommandKey command;
    Addr pc = 0;
    VectorConfig config;
    VectorCommandPayload payload;
    RequestorID requestorId = Request::invldRequestorId;
};

inline bool
operator==(const VectorCommand &lhs, const VectorCommand &rhs)
{
    return lhs.command == rhs.command && lhs.pc == rhs.pc &&
           lhs.config == rhs.config && lhs.payload == rhs.payload &&
           lhs.requestorId == rhs.requestorId;
}

inline bool
operator!=(const VectorCommand &lhs, const VectorCommand &rhs)
{
    return !(lhs == rhs);
}

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMMAND_HH__
