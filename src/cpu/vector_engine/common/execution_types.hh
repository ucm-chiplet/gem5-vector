// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_EXECUTION_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_EXECUTION_TYPES_HH__

#include <optional>
#include <vector>

#include "cpu/vector_engine/common/fp_types.hh"
#include "cpu/vector_engine/common/lane_task.hh"

namespace gem5::vector_engine
{

/** Operandos leídos por la lane, con contexto FP sólo para la FPU. */
struct ExecutionBundle
{
    LaneFragmentKey key;
    ArithmeticOperation operation = ArithmeticOperation::Invalid;
    uint16_t sewBits = 0;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    // Contenedores de patrones de bits; sólo los sewBits bajos son activos.
    std::vector<uint32_t> lhs;
    std::vector<uint32_t> rhs;
    std::optional<FpExecutionContext> fpContext = std::nullopt;
};

/** Resultado retenido hasta el writeback; los flags no son faults. */
struct ExecutionResult
{
    LaneFragmentKey key;
    uint16_t sewBits = 0;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    std::vector<uint32_t> values;
    FpExceptionFlags fpFlags = 0;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_EXECUTION_TYPES_HH__
