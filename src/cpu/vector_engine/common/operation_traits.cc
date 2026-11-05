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

namespace
{

bool
containsWidth(const OperationWidths &widths, uint16_t bits)
{
    return bits &&
           std::find(widths.begin(), widths.end(), bits) != widths.end();
}

/**
 * Ficha común de las operaciones enteras de igual ancho: ALU, 8/16/32/64
 * bits, formas .vv y .vx, sin contexto FP y admisibles desde CPU en todos
 * sus anchos. Sólo cambia si el comando debe declarar el signo.
 */
constexpr OperationTraits
integerTraits(bool requires_signedness)
{
    return OperationTraits{ExecutionClass::IntegerAlu,
                           ElementFormat::Integer,
                           IntegerElementWidths,
                           true,  // Dos fuentes vectoriales (.vv).
                           true,  // Vector y escalar capturado (.vx).
                           false, // No necesita redondeo FP.
                           requires_signedness,
                           false, // No accede a memoria.
                           IntegerElementWidths};
}

/**
 * Ficha de coma flotante de igual ancho: FPU, sólo forma .vv, contexto de
 * redondeo obligatorio y sin signo de comando (el signo es del formato).
 */
constexpr OperationTraits
floatTraits(OperationWidths widths, OperationWidths offload_widths)
{
    return OperationTraits{ExecutionClass::FloatingPoint,
                           ElementFormat::IeeeFloat,
                           widths,
                           true,  // Dos fuentes vectoriales (.vv).
                           false, // Las formas .vf quedan fuera del hito 2.
                           true,  // Necesita el redondeo capturado.
                           false, // Sin signo de comando.
                           false, // No accede a memoria.
                           offload_widths};
}

} // anonymous namespace

bool
OperationTraits::supportsWidth(uint16_t bits) const
{
    return containsWidth(widths, bits);
}

bool
OperationTraits::offloadsWidth(uint16_t bits) const
{
    return containsWidth(offloadWidths, bits) && supportsWidth(bits);
}

bool
OperationTraits::acceptsSignedness(ElementSignedness signedness) const
{
    if (requiresSignedness) {
        return signedness == ElementSignedness::Signed ||
               signedness == ElementSignedness::Unsigned;
    }
    return signedness == ElementSignedness::NotApplicable;
}

const OperationTraits *
operationTraits(ArithmeticOperation operation)
{
    // Referencias semánticas: RVV 1.0, edición 20240411, secciones Vector
    // Single-Width Integer Add and Subtract, Vector Single-Width Integer
    // Multiply Instructions, Vector Integer Divide Instructions, Vector
    // Single-Width Floating-Point Add/Subtract Instructions y Vector
    // Single-Width Floating-Point Multiply/Divide Instructions.
    static constexpr OperationTraits add = integerTraits(false);
    // vmul: bits bajos; idénticos con o sin signo, así que no se declara.
    static constexpr OperationTraits multiply = integerTraits(false);
    // vdiv/vdivu y vrem/vremu comparten ficha; el signo los distingue.
    static constexpr OperationTraits divide = integerTraits(true);
    static constexpr OperationTraits remainder = integerTraits(true);
    // binary16 es una capacidad interna; CPU sólo puede pedir binary32.
    static constexpr OperationTraits float_add = floatTraits({16, 32}, {32});
    static constexpr OperationTraits float_multiply = floatTraits({32}, {32});
    static constexpr OperationTraits float_divide = floatTraits({32}, {32});
    switch (operation) {
        case ArithmeticOperation::Add:
            return &add;
        case ArithmeticOperation::Multiply:
            return &multiply;
        case ArithmeticOperation::Divide:
            return &divide;
        case ArithmeticOperation::Remainder:
            return &remainder;
        case ArithmeticOperation::FloatAdd:
            return &float_add;
        case ArithmeticOperation::FloatMultiply:
            return &float_multiply;
        case ArithmeticOperation::FloatDivide:
            return &float_divide;
        default:
            return nullptr;
    }
}

const OperationTraits *
operationTraits(MemoryDirection direction)
{
    static constexpr OperationTraits memory{
        ExecutionClass::Memory,
        ElementFormat::RawBits,
        IntegerElementWidths,
        false,                 // No es una forma .vv.
        false,                 // No es una forma .vx.
        false,                 // No necesita redondeo FP.
        false,                 // Sin signo de comando.
        true,                  // Puede fallar un acceso.
        IntegerElementWidths}; // Admisible desde CPU en todos los anchos.
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
