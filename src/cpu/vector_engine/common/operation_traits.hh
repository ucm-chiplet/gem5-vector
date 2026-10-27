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
inline constexpr std::array<uint16_t, 4> IntegerElementWidths{8, 16, 32, 64};

/** Indica qué tipo de unidad se encarga de una operación enviada por CPU. */
enum class ExecutionClass : uint8_t
{
    IntegerAlu,
    FloatingPoint,
    Memory
};

/**
 * Ficha de lo que sabe hacer cada operación: unidad, tamaños y fuentes.
 * cpuOffload permite admitir el comando desde CPU; no garantiza que Minor
 * sepa construir todas sus variantes. Un cero en widths deja una posición
 * sin usar. La ficha no indica si la unidad está libre en este momento.
 */
struct OperationTraits
{
    ExecutionClass execution;
    ElementFormat format;
    std::array<uint16_t, IntegerElementWidths.size()> widths;
    bool vectorVector;
    bool vectorScalar;
    bool requiresFpContext;
    bool mayMemoryFault;
    bool cpuOffload;

    /** Indica si esta operación admite elementos de ese número de bits. */
    bool supportsWidth(uint16_t bits) const;
};

/** Busca la ficha de cálculo; devuelve nullptr si no conoce la operación.
 * La ficha es compartida y permanece válida durante toda la simulación. */
const OperationTraits *operationTraits(ArithmeticOperation operation);
/** Busca la ficha de carga/store; devuelve nullptr para otras direcciones.
 */
const OperationTraits *operationTraits(MemoryDirection direction);

/**
 * Enumera qué lee y qué escribe un comando ya validado. Incluye el ancho
 * de cada elemento (EEW) y el tamaño de su grupo de registros (EMUL).
 * La lista se construye a partir del comando y pertenece a quien la pide;
 * no se mantiene una segunda copia que haya que actualizar por separado.
 */
std::vector<OperandDescriptor> describeOperands(const VectorCommand &command);

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_OPERATION_TRAITS_HH__
