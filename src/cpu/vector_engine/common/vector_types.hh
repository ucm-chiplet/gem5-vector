#ifndef __CPU_VECTOR_ENGINE_COMMON_VECTOR_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_VECTOR_TYPES_HH__

#include <cstdint>

namespace gem5::vector_engine
{

/** Base-two exponent used to encode the effective RVV LMUL. */
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

/** Effective RVV state captured by MinorCPU when it creates a command. */
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

enum class ArithmeticOperation : uint8_t
{
    Invalid,
    Add,
};

enum class ElementWidthMode : uint8_t
{
    SameWidth,
    Widening,
    Narrowing,
};

enum class ElementSignedness : uint8_t
{
    NotApplicable,
    Signed,
    Unsigned,
};

enum class MemoryDirection : uint8_t
{
    Invalid,
    Load,
    Store,
};

enum class MemoryOrdering : uint8_t
{
    NotApplicable,
    Ordered,
    Unordered,
};

/** Backend unit which owns a task derived from a vector command. */
enum class VectorUnitClass : uint8_t
{
    Invalid,
    Lanes,
    Vlsu,
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VECTOR_TYPES_HH__
