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
 * Trabajo que una lane entrega a la ALU o FPU con sus operandos ya leídos.
 * La unidad copia estos datos al aceptar, por lo que no necesita leer el VRF.
 * byteOrder indica cómo interpretar los bytes y emul conserva el tamaño del
 * grupo de registros. Si la unidad pide reintentar, la lane guarda el mismo
 * mensaje y su identidad para volver a enviarlo.
 */
struct ExecutionBundle
{
    LaneFragmentKey key;
    ArithmeticOperation operation = ArithmeticOperation::Invalid;
    uint16_t sewBits = 0;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    // Elementos consecutivos, con elementCount * bytes(SEW) en cada fuente.
    ByteBuffer lhs;
    ByteBuffer rhs;
    std::optional<FpExecutionContext> fpContext = std::nullopt;
    ByteOrder byteOrder = ByteOrder::little;
    VectorLmul emul = VectorLmul::Invalid;
};

/**
 * Resultado que la lane guarda hasta que el VRF confirme su escritura.
 * values tiene destinationRange.size bytes; orden y EMUL deben coincidir
 * con el trabajo original. fpFlags reúne los indicadores de coma flotante
 * de sus elementos; no representa un fallo de acceso a memoria.
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
