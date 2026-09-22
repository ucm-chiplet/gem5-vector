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

#ifndef __CPU_VECTOR_ENGINE_FRONTEND_TASK_DISTRIBUTOR_HH__
#define __CPU_VECTOR_ENGINE_FRONTEND_TASK_DISTRIBUTOR_HH__

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/lane_completion.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Reparte una tarea aritmética activa en fragmentos contiguos por lane.
 * El propietario conecta los callbacks al sequencer y a las lanes; una
 * aceptación transfiere una copia y obliga a emitir una sola finalización.
 * Las lanes sólo responden después de confirmar sus escrituras en el VRF.
 *
 * Se permiten respuestas síncronas durante el envío. Los callbacks no
 * destruyen el módulo. Propietario, mapper y receptores deben sobrevivirlo;
 * su destrucción no sustituye drain ni cancela trabajo aceptado.
 */
class TaskDistributor
{
  public:
    using LaneSender = std::function<TransferResult(const LaneTask &)>;
    using CompletionSender = std::function<void(const UnitCompletion &)>;

    TaskDistributor(ClockedObject &owner, const AddressMapper &mapper,
                    LaneSender send_lane, CompletionSender send_completion);
    ~TaskDistributor();

    TaskDistributor(const TaskDistributor &) = delete;
    TaskDistributor &operator=(const TaskDistributor &) = delete;

    TransferResult acceptTask(const ArithmeticTask &task);
    void recvCompletion(const LaneCompletion &completion);

    // Programa evaluación en el siguiente ciclo, nunca envía directamente.
    void wakeup();

    // Estado local; el propietario coordina el drain conjunto de la VPU.
    bool
    isIdle() const
    {
        return !active && !progressEvent.scheduled();
    }

  private:
    enum class FragmentPhase
    {
        Pending,
        Sending,
        Accepted,
        Completed,
    };

    struct FragmentState
    {
        LaneTask task;
        FragmentPhase phase = FragmentPhase::Pending;
    };

    struct DistributionState
    {
        ArithmeticTask task;

        // El índice es fragmentId; las entradas viven hasta cerrar la tarea.
        // Su tamaño es el siguiente identificador libre durante el reparto.
        std::vector<FragmentState> fragments;
        std::vector<std::deque<LaneFragmentId>> pendingByLane;
        std::size_t unfinishedFragments = 0;
    };

    ClockedObject &owner;
    const AddressMapper &mapper;
    LaneSender sendLane;
    CompletionSender sendCompletion;
    std::optional<DistributionState> active;
    EventFunctionWrapper progressEvent;

    void validateTask(const ArithmeticTask &task) const;
    DistributionState makeDistribution(const ArithmeticTask &task) const;
    bool hasPending() const;
    void evaluate();
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_FRONTEND_TASK_DISTRIBUTOR_HH__
