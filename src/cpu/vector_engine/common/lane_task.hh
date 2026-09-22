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

#ifndef __CPU_VECTOR_ENGINE_COMMON_LANE_TASK_HH__
#define __CPU_VECTOR_ENGINE_COMMON_LANE_TASK_HH__

#include <cstdint>
#include <limits>
#include <optional>

#include "cpu/vector_engine/common/unit_task.hh"
#include "cpu/vector_engine/common/vrf_types.hh"
#include "cpu/vector_engine/interface/vector_command.hh"

namespace gem5::vector_engine
{

using LaneFragmentId = uint32_t;
inline constexpr LaneFragmentId InvalidLaneFragmentId =
    std::numeric_limits<LaneFragmentId>::max();

/** El distribuidor asigna fragmentId sin sustituir la identidad de tarea. */
struct LaneFragmentKey
{
    TaskKey task;
    LaneFragmentId fragmentId = InvalidLaneFragmentId;

    constexpr bool
    valid() const
    {
        return task.valid() && fragmentId != InvalidLaneFragmentId;
    }
};

constexpr bool
operator==(const LaneFragmentKey &lhs, const LaneFragmentKey &rhs)
{
    return lhs.task == rhs.task && lhs.fragmentId == rhs.fragmentId;
}

constexpr bool
operator!=(const LaneFragmentKey &lhs, const LaneFragmentKey &rhs)
{
    return !(lhs == rhs);
}

/**
 * Elementos contiguos de una sola palabra de cada operando vectorial.
 * Todos los accesos pertenecen a laneId; los rangos son relativos al grupo
 * arquitectónico, no al primer elemento activo ni a la palabra local.
 */
struct LaneTask
{
    LaneFragmentKey key;
    LaneId laneId = 0;
    ArithmeticCommand arithmetic;
    ElementRange elements;
    ByteRange destinationRange;
    ByteRange vectorSourceRange;

    // Ausente para vadd.vx: el escalar viaja en arithmetic.secondOperand.
    std::optional<ByteRange> secondVectorSourceRange;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_LANE_TASK_HH__
