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

#ifndef __CPU_VECTOR_ENGINE_FRONTEND_ARA_SEQUENCER_HH__
#define __CPU_VECTOR_ENGINE_FRONTEND_ARA_SEQUENCER_HH__

#include <optional>
#include <variant>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/unit_completion.hh"

namespace gem5::vector_engine::detail
{

/**
 * AraSequencer conserva aquí el comando que está ejecutando y su tarea.
 * En la versión inicial sólo puede haber una entrada activa.
 */
struct ActiveCommandState
{
    VectorCommand command;

    // AraSequencer crea una tarea para [vstart, vl), salvo si está vacío.
    // Conserva el mismo descriptor y TaskKey cuando la unidad devuelve Retry.
    std::optional<std::variant<ArithmeticTask, MemoryTask>> task;

    // La unidad la ha aceptado, pero no terminado; AraSequencer no la reenvía.
    bool taskAccepted = false;

    // La unidad responde cuando terminan los accesos y las escrituras.
    // AraSequencer comprueba que TaskKey corresponde a la tarea conservada.
    std::optional<UnitCompletion> completion;
};

} // namespace gem5::vector_engine::detail

#endif // __CPU_VECTOR_ENGINE_FRONTEND_ARA_SEQUENCER_HH__
