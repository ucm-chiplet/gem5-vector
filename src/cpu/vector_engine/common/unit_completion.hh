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

#ifndef __CPU_VECTOR_ENGINE_COMMON_UNIT_COMPLETION_HH__
#define __CPU_VECTOR_ENGINE_COMMON_UNIT_COMPLETION_HH__

#include <cstdint>
#include <optional>

#include "cpu/vector_engine/common/unit_task.hh"
#include "cpu/vector_engine/interface/vector_completion.hh"

namespace gem5::vector_engine
{

/** AraSequencer distingue una tarea terminada de un fallo de memoria. */
enum class UnitCompletionStatus : uint8_t
{
    Success,
    MemoryFault,
};

/**
 * TaskDistributor o AraVLSU la envían a AraSequencer al terminar una tarea.
 * La clave permite asociar la respuesta con la tarea pendiente.
 */
struct UnitCompletion
{
    TaskKey key;
    UnitCompletionStatus status = UnitCompletionStatus::Success;

    // AraVLSU informa de causa, dirección e índice si hay MemoryFault.
    // TaskDistributor sólo devuelve Success en la versión inicial.
    // AraSequencer exige que fault esté ausente en una respuesta Success.
    std::optional<FaultInfo> fault;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_UNIT_COMPLETION_HH__
