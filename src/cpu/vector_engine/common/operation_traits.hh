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

#ifndef __CPU_VECTOR_ENGINE_COMMON_OPERATION_TRAITS_HH__
#define __CPU_VECTOR_ENGINE_COMMON_OPERATION_TRAITS_HH__

#include <array>
#include <vector>

#include "cpu/vector_engine/common/operand_types.hh"

namespace gem5::vector_engine
{

struct VectorCommand;

// Anchos del backend actual, compartidos por suma y memoria; no son la
// matriz objetivo SEW x LMUL ni una lista de rutas de decode comprobadas.
inline constexpr std::array<uint16_t, 2> IntegerElementWidths{16, 32};

/** Recurso funcional que consume la operación ya decodificada por CPU. */
enum class ExecutionClass : uint8_t
{
    IntegerAlu,
    FloatingPoint,
    Memory
};

/**
 * Metadatos compartidos del soporte implementado, sin capacidad temporal.
 * cpuOffload significa admisible en la frontera, no que Minor traduzca ya
 * todas las formas/anchos. Las entradas widths con cero no son formatos.
 */
struct OperationTraits
{
    ExecutionClass execution;
    ElementFormat format;
    std::array<uint16_t, 2> widths;
    bool vectorVector;
    bool vectorScalar;
    bool requiresFpContext;
    bool mayMemoryFault;
    bool cpuOffload;

    /** Consulta anchos efectivos de esta operación; nunca acepta cero. */
    bool supportsWidth(uint16_t bits) const;
};

/** Devuelve metadatos estáticos inmutables, o nullptr para una operación
 * ajena. */
const OperationTraits *operationTraits(ArithmeticOperation operation);
/** Cargas y stores comparten transporte; otras direcciones devuelven nullptr.
 */
const OperationTraits *operationTraits(MemoryDirection direction);

/**
 * Deriva roles y EEW/EMUL del perfil de igual ancho ya validado. El caller
 * recibe su propia colección; no conserva referencias a buffers temporales
 * ni decodifica opcodes. Los stores aportan una fuente, no un destino.
 */
std::vector<OperandDescriptor> describeOperands(const VectorCommand &command);

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_OPERATION_TRAITS_HH__
