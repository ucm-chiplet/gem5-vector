// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_EXECUTION_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_EXECUTION_TYPES_HH__

#include <optional>

#include "cpu/vector_engine/common/fp_types.hh"
#include "cpu/vector_engine/common/lane_task.hh"
#include "enums/ByteOrder.hh"

namespace gem5::vector_engine
{

/**
 * Operandos propios de un fragmento, preparados por la lane antes de enviar.
 * La FU copia datos al aceptar y no lee el VRF. emul conserva el grupo
 * efectivo; byteOrder permite interpretar bytes sin depender del host.
 * Un rechazo deja el mismo bundle e identidad en manos de la lane.
 */
struct ExecutionBundle
{
    LaneFragmentKey key;
    ArithmeticOperation operation = ArithmeticOperation::Invalid;
    uint16_t sewBits = 0;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    // Increasing element and byte offset; size = count * bytes(SEW).
    ByteBuffer lhs;
    ByteBuffer rhs;
    std::optional<FpExecutionContext> fpContext = std::nullopt;
    ByteOrder byteOrder = ByteOrder::little;
    VectorLmul emul = VectorLmul::Invalid;
};

/**
 * Resultado que la lane retiene hasta confirmar el writeback del fragmento.
 * values tiene destinationRange.size bytes; orden y EMUL deben coincidir
 * con el bundle original. Los flags son efectos FP agregados, no faults.
 */
struct ExecutionResult
{
    LaneFragmentKey key;
    uint16_t sewBits = 0;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    ByteBuffer values;
    FpExceptionFlags fpFlags = 0;
    ByteOrder byteOrder = ByteOrder::little;
    VectorLmul emul = VectorLmul::Invalid;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_EXECUTION_TYPES_HH__
