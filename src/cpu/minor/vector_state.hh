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

#ifndef __CPU_MINOR_VECTOR_STATE_HH__
#define __CPU_MINOR_VECTOR_STATE_HH__

#include <optional>

#include "cpu/minor/dyn_inst.hh"
#include "cpu/minor/vector_types.hh"
#include "cpu/vector_engine/interface/cpu_vector_interface.hh"

namespace gem5::minor
{

/** Execute usa estas fases para seguir el envío y la respuesta de la VPU. */
enum class MinorVectorPhase : uint8_t
{
    WaitDependencies, // Execute aún no ha capturado los operandos.
    WaitGrant,        // Conserva el comando ante una respuesta Stall.
    Dispatched,       // Ha consumido la reserva; espera accepted.
    Accepted,         // Espera la respuesta final del comando.
    Completed,        // Debe procesar el resultado o la excepción.
};

/**
 * Estado que Execute conserva hasta retirar la instrucción o tratar su fallo.
 * Execute es su propietario, no la instrucción a la que apunta inst.
 * Describe el contrato; las transiciones aún no están conectadas al pipeline.
 */
struct MinorVectorState
{
    MinorDynInstPtr inst;
    MinorVectorPhase phase = MinorVectorPhase::WaitDependencies;
    DecodedVectorOp decoded;

    // Execute asocia con esta clave el comando, la reserva y la respuesta.
    std::optional<vector_engine::CommandKey> commandKey;

    // Execute lo captura una vez y puede liberarlo tras recibir accepted.
    std::optional<vector_engine::VectorCommand> command;

    // Execute recibe la reserva y la consume una sola vez al hacer dispatch.
    std::optional<vector_engine::GrantToken> grantToken;

    // Minor la conserva hasta procesar el retiro o la excepción.
    std::optional<vector_engine::VectorCompletion> completion;
};

} // namespace gem5::minor

#endif // __CPU_MINOR_VECTOR_STATE_HH__
