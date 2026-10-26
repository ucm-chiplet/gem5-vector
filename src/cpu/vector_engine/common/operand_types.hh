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

#ifndef __CPU_VECTOR_ENGINE_COMMON_OPERAND_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_OPERAND_TYPES_HH__

#include <variant>

#include "base/types.hh"
#include "cpu/vector_engine/common/vector_reg_ref.hh"
#include "cpu/vector_engine/common/vector_types.hh"

namespace gem5::vector_engine
{

/** Para qué usa la operación este dato: fuente, resultado, dirección, etc. */
enum class OperandRole : uint8_t
{
    Source,
    Destination,
    Accumulator,
    Predicate,
    Address,
};

/** Indica si la operación lee el dato, lo escribe o hace ambas cosas. */
enum class OperandAccess : uint8_t
{
    Read,
    Write,
    ReadWrite
};
/** Cómo interpretar los bits; las cargas/stores sólo los copian (RawBits). */
enum class ElementFormat : uint8_t
{
    RawBits,
    Integer,
    IeeeFloat,
    Mask
};

/** Copia de un valor escalar que la CPU ya leyó al crear el comando. */
struct CapturedScalar
{
    uint64_t bits = 0;
};

/** Registro escalar donde guardar un resultado en una ampliación futura. */
struct ScalarDestination
{
    RegIndex index = 0;
};

/**
 * Descripción de un dato leído o escrito, obtenida a partir del comando.
 * eewBits es el ancho del elemento y emul el tamaño del grupo de registros.
 * Un valor escalar no tiene grupo (EMUL Invalid); una dirección tampoco es
 * un elemento vectorial (EEW=0). Acumuladores, predicados y destinos escalares
 * están reservados para operaciones futuras.
 */
struct OperandDescriptor
{
    OperandRole role;
    OperandAccess access;
    ElementFormat format;
    uint16_t eewBits;
    VectorLmul emul;
    std::variant<VectorRegRef, CapturedScalar, ScalarDestination> value;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_OPERAND_TYPES_HH__
