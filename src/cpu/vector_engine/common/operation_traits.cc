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

#include "cpu/vector_engine/common/operation_traits.hh"

#include <algorithm>

#include "cpu/vector_engine/interface/vector_command.hh"

namespace gem5::vector_engine
{

bool
OperationTraits::supportsWidth(uint16_t bits) const
{
    return bits &&
           std::find(widths.begin(), widths.end(), bits) != widths.end();
}

const OperationTraits *
operationTraits(ArithmeticOperation operation)
{
    static constexpr OperationTraits add{ExecutionClass::IntegerAlu,
                                         ElementFormat::Integer,
                                         IntegerElementWidths,
                                         true,  // Dos fuentes vectoriales.
                                         true,  // Vector y escalar.
                                         false, // No necesita redondeo FP.
                                         false, // No accede a memoria.
                                         true}; // Admisible desde CPU.
    static constexpr OperationTraits float_add{ExecutionClass::FloatingPoint,
                                               ElementFormat::IeeeFloat,
                                               {16},   // Sólo binary16.
                                               true,   // Dos vectores.
                                               false,  // Sin forma escalar.
                                               true,   // Necesita redondeo.
                                               false,  // Sin acceso a memoria.
                                               false}; // Sin offload CPU.
    switch (operation) {
        case ArithmeticOperation::Add:
            return &add;
        case ArithmeticOperation::FloatAdd:
            return &float_add;
        default:
            return nullptr;
    }
}

const OperationTraits *
operationTraits(MemoryDirection direction)
{
    static constexpr OperationTraits memory{ExecutionClass::Memory,
                                            ElementFormat::RawBits,
                                            IntegerElementWidths,
                                            false, // No es suma .vv.
                                            false, // No es suma .vx.
                                            false, // No necesita redondeo FP.
                                            true,  // Puede fallar un acceso.
                                            true}; // Admisible desde CPU.
    if (direction == MemoryDirection::Load ||
        direction == MemoryDirection::Store) {
        return &memory;
    }
    return nullptr;
}

std::vector<OperandDescriptor>
describeOperands(const VectorCommand &command)
{
    std::vector<OperandDescriptor> operands;
    const auto &config = command.config;
    if (const auto *arithmetic =
            std::get_if<ArithmeticCommand>(&command.payload)) {
        const auto *traits = operationTraits(arithmetic->operation);
        if (!traits) {
            return operands;
        }
        // Todos los registros de estas operaciones usan el ancho y el
        // grupo del comando. Sólo cambian su papel y si se leen o escriben.
        const auto append = [&](OperandRole role, OperandAccess access,
                                const VectorRegRef &reg) {
            operands.push_back({role, access, traits->format, config.sewBits,
                                config.lmul, reg});
        };
        append(OperandRole::Destination, OperandAccess::Write,
               arithmetic->destination);
        append(OperandRole::Source, OperandAccess::Read,
               arithmetic->vectorSource);
        if (const auto *reg =
                std::get_if<VectorRegRef>(&arithmetic->secondOperand)) {
            append(OperandRole::Source, OperandAccess::Read, *reg);
        } else if (const auto *scalar =
                       std::get_if<RegVal>(&arithmetic->secondOperand)) {
            // El escalar ya se leyó en CPU; no pertenece a un grupo vectorial.
            operands.push_back({OperandRole::Source, OperandAccess::Read,
                                traits->format, config.sewBits,
                                VectorLmul::Invalid, CapturedScalar{*scalar}});
        }
    } else if (const auto *memory =
                   std::get_if<MemoryCommand>(&command.payload)) {
        // Una carga escribe el registro; un store lee sus datos.
        auto role = OperandRole::Source;
        auto access = OperandAccess::Read;
        if (memory->direction == MemoryDirection::Load) {
            role = OperandRole::Destination;
            access = OperandAccess::Write;
        }
        // En el perfil actual, el elemento de memoria usa el ancho del
        // comando y ocupa el mismo grupo de registros.
        operands.push_back({role, access, ElementFormat::RawBits,
                            memory->elementWidthBits, config.lmul,
                            memory->dataReg});
        // La dirección se guarda como valor: no es un elemento del vector.
        operands.push_back({OperandRole::Address, OperandAccess::Read,
                            ElementFormat::Integer, 0, VectorLmul::Invalid,
                            CapturedScalar{memory->base}});
    }
    return operands;
}

} // namespace gem5::vector_engine
