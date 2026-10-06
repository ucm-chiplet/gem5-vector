// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_FP_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_FP_TYPES_HH__

#include <cstdint>

namespace gem5::vector_engine
{

/** Modo ya resuelto por el productor; no contiene un selector dinámico. */
enum class FpRoundingMode : uint8_t
{
    NearestEven = 0,
    TowardZero = 1,
    Down = 2,
    Up = 3,
    NearestMaxMagnitude = 4,
    Invalid = 255,
};

constexpr bool
isValid(FpRoundingMode mode)
{
    return mode >= FpRoundingMode::NearestEven &&
           mode <= FpRoundingMode::NearestMaxMagnitude;
}

/** Entrada explícita de la ejecución FP interna, capturada antes de Retry. */
struct FpExecutionContext
{
    FpRoundingMode roundingMode = FpRoundingMode::Invalid;
};

// Codificación de fflags RVV. Se devuelve el OR de los elementos ejecutados;
// el consumidor arquitectónico debe acumularlo en el CSR correspondiente.
using FpExceptionFlags = uint8_t;
inline constexpr FpExceptionFlags FpInexact = 1;
inline constexpr FpExceptionFlags FpUnderflow = 2;
inline constexpr FpExceptionFlags FpOverflow = 4;
inline constexpr FpExceptionFlags FpDivideByZero = 8;
inline constexpr FpExceptionFlags FpInvalid = 16;
inline constexpr FpExceptionFlags FpFlagsMask =
    FpInexact | FpUnderflow | FpOverflow | FpDivideByZero | FpInvalid;

constexpr bool
validFpFlags(FpExceptionFlags flags)
{
    return (flags & ~FpFlagsMask) == 0;
}

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_FP_TYPES_HH__
