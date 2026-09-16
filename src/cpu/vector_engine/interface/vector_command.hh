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
// Arithmetic command types.
// --------------------------------------------------------------------------

/**
 * The active alternative identifies vv, vx, or vi arithmetic without a
 * second, potentially contradictory, operand-form field.
 */
using ArithmeticOperand = std::variant<VectorRegRef, RegVal, int64_t>;

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

// End of arithmetic command types.

// --------------------------------------------------------------------------
// Memory addressing types.
// --------------------------------------------------------------------------

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

using MemoryAddressing = std::variant<
    UnitStrideAddress,
    StridedAddress,
    IndexedAddress>;

// End of memory addressing types.

// --------------------------------------------------------------------------
// Memory command type.
// --------------------------------------------------------------------------

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

// End of memory command type.

// --------------------------------------------------------------------------
// Common command payload.
// --------------------------------------------------------------------------

using VectorCommandPayload =
    std::variant<ArithmeticCommand, MemoryCommand>;

// End of common command payload.

// --------------------------------------------------------------------------
// CPU--VPU command descriptor.
// --------------------------------------------------------------------------

/**
 * Immutable descriptor at the CPU--VPU boundary.
 *
 * MinorCPU supplies already-decoded semantics and effective operand values.
 * The VPU must not use this descriptor to recover a StaticInst or DynInst.
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

// End of CPU--VPU command descriptor.

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMMAND_HH__
