/*
 * Copyright (c) 2026 Félix Garcia Narocki (UCM)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_MINOR_VECTOR_TYPES_HH__
#define __CPU_MINOR_VECTOR_TYPES_HH__

#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

#include "base/types.hh"
#include "cpu/vector_engine/common/vector_types.hh"

namespace gem5::minor
{

/** Decode clasifica la instrucción; Execute elige su ruta de ejecución. */
enum class VectorExecutionClass : uint8_t
{
    CpuScalar,
    CpuVectorConfig,
    VpuOffload,
    NativeVector,
};

/**
 * Decode identifica una fuente escalar por su índice en StaticInst.
 * Execute usa ese índice para leer el operando cuando esté disponible.
 * No es el número de un registro arquitectónico ni su valor capturado.
 */
struct ScalarOperandRef
{
    uint32_t sourceOperandIndex = std::numeric_limits<uint32_t>::max();
};

/**
 * Operación aritmética que Decode describe y Execute convierte en comando.
 * Los registros son números arquitectónicos; Execute dimensiona sus grupos.
 */
struct DecodedArithmeticOp
{
    vector_engine::ArithmeticOperation operation =
        vector_engine::ArithmeticOperation::Invalid;
    RegIndex destination = 0;
    RegIndex vectorSource = 0;
    std::variant<RegIndex, ScalarOperandRef> secondOperand;
};

/**
 * Carga o store que Decode describe para Execute.
 * Execute resuelve la base escalar y el grupo vectorial antes del envío.
 */
struct DecodedMemoryOp
{
    vector_engine::MemoryDirection direction =
        vector_engine::MemoryDirection::Invalid;
    RegIndex dataReg = 0;
    ScalarOperandRef base;
    uint16_t elementWidthBits = 0;
};

/**
 * Descripción que Decode entrega a Execute sin repetir la decodificación.
 * Permanece en MinorCPU; las unidades de la VPU reciben VectorCommand.
 */
struct DecodedVectorOp
{
    VectorExecutionClass executionClass = VectorExecutionClass::CpuScalar;

    // Execute copia la predicación de la instrucción a VectorConfig.
    bool masked = false;

    // Sólo VpuOffload lleva payload; las demás clases siguen sus rutas CPU.
    std::optional<std::variant<DecodedArithmeticOp, DecodedMemoryOp>> payload;
};

} // namespace gem5::minor

#endif // __CPU_MINOR_VECTOR_TYPES_HH__
