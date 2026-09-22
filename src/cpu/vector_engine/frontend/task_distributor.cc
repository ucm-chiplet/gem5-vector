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

#include "cpu/vector_engine/frontend/task_distributor.hh"

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>

#include "base/logging.hh"

namespace gem5::vector_engine
{

TaskDistributor::TaskDistributor(ClockedObject &owner,
                                 const AddressMapper &mapper,
                                 LaneSender send_lane,
                                 CompletionSender send_completion)
    : owner(owner),
      mapper(mapper),
      sendLane(std::move(send_lane)),
      sendCompletion(std::move(send_completion)),
      progressEvent([this] { evaluate(); }, owner.name() + ".task_distributor")
{
    fatal_if(!sendLane || !sendCompletion,
             "TaskDistributor requires lane and completion senders");
}

TaskDistributor::~TaskDistributor()
{
    if (progressEvent.scheduled()) {
        owner.deschedule(progressEvent);
    }
}

void
TaskDistributor::validateTask(const ArithmeticTask &task) const
{
    const auto &unit = task.task;
    const auto &config = task.config;
    const auto &arithmetic = task.arithmetic;

    panic_if(!unit.valid() || unit.unit != VectorUnitClass::Lanes,
             "Invalid arithmetic task identity or unit");
    panic_if(!isValid(config.lmul) || config.sewBits != 32 || config.masked ||
                 !unit.elements.fitsWithin(config.vstart, config.vl),
             "Invalid arithmetic task configuration or active elements");
    panic_if(arithmetic.operation != ArithmeticOperation::Add ||
                 arithmetic.widthMode != ElementWidthMode::SameWidth ||
                 arithmetic.signedness != ElementSignedness::NotApplicable,
             "Unsupported operation in an admitted arithmetic task");

    const auto *second_source =
        std::get_if<VectorRegRef>(&arithmetic.secondOperand);
    panic_if(!second_source &&
                 !std::holds_alternative<RegVal>(arithmetic.secondOperand),
             "Arithmetic task requires a vector or scalar second operand");

    const uint64_t offset = uint64_t{unit.elements.firstElement} * 4;
    const uint64_t size = uint64_t{unit.elements.elementCount} * 4;
    const uint64_t end = offset + size;
    panic_if(end > std::numeric_limits<uint32_t>::max() ||
                 task.destinationRange.offset != offset ||
                 task.destinationRange.size != size,
             "Arithmetic byte range does not match its elements");

    const auto vlen_bytes = mapper.geometry().vlenBytes;
    const int exponent = static_cast<int>(config.lmul);
    const uint64_t effective_bytes = exponent >= 0
                                         ? uint64_t{vlen_bytes} << exponent
                                         : uint64_t{vlen_bytes} >> -exponent;
    panic_if(uint64_t{config.vl} * 4 > effective_bytes ||
                 end > effective_bytes,
             "Arithmetic task exceeds effective LMUL capacity");

    const unsigned expected_regs = exponent > 0 ? 1U << exponent : 1U;
    const auto check_register = [&](const VectorRegRef &reg) {
        panic_if(!reg.naturallyAligned() || reg.regCount != expected_regs ||
                     end > uint64_t{vlen_bytes} * reg.regCount,
                 "Arithmetic task has an invalid register group");
    };
    check_register(arithmetic.destination);
    check_register(arithmetic.vectorSource);
    if (second_source) {
        check_register(*second_source);
    }
}

TaskDistributor::DistributionState
TaskDistributor::makeDistribution(const ArithmeticTask &task) const
{
    DistributionState state;
    state.task = task;
    state.pendingByLane.resize(mapper.geometry().numLanes);

    const auto &arithmetic = task.arithmetic;
    const auto &range = task.destinationRange;
    const ByteEnable byte_enable(range.size, 1);
    std::vector<VrfMapping> mappings;
    mappings.push_back(mapper.map(arithmetic.destination, range, byte_enable));
    mappings.push_back(
        mapper.map(arithmetic.vectorSource, range, byte_enable));
    if (const auto *source =
            std::get_if<VectorRegRef>(&arithmetic.secondOperand)) {
        mappings.push_back(mapper.map(*source, range, byte_enable));
    }

    // Recorrer conjuntamente destino y fuentes, cortando en cada frontera.
    std::vector<std::size_t> positions(mappings.size(), 0);
    uint32_t offset = range.offset;
    while (offset < range.end()) {
        panic_if(positions.front() >= mappings.front().size(),
                 "Destination mapping has a coverage gap");
        const LaneId lane = mappings.front()[positions.front()].laneId;
        uint32_t end = range.end();

        for (std::size_t i = 0; i < mappings.size(); ++i) {
            panic_if(positions[i] >= mappings[i].size(),
                     "Operand mapping has a coverage gap");
            const auto &mapped = mappings[i][positions[i]];
            panic_if(!mapped.originalRange.valid() ||
                         offset < mapped.originalRange.offset ||
                         offset >= mapped.originalRange.end() ||
                         mapped.laneId != lane,
                     "Operand mapping does not belong to the same lane");
            end = std::min(end, mapped.originalRange.end());
        }

        panic_if(offset % 4 || end % 4 || end <= offset ||
                     lane >= state.pendingByLane.size(),
                 "Mapped fragment does not contain whole lane elements");
        panic_if(state.fragments.size() >= InvalidLaneFragmentId,
                 "Lane fragment identifiers exhausted");
        const auto fragment_id =
            static_cast<LaneFragmentId>(state.fragments.size());
        const ByteRange fragment_range{offset, end - offset};

        LaneTask fragment;
        fragment.key = LaneFragmentKey{task.task.key, fragment_id};
        fragment.laneId = lane;
        fragment.arithmetic = arithmetic;
        fragment.elements = ElementRange{offset / 4, (end - offset) / 4};
        fragment.destinationRange = fragment_range;
        fragment.vectorSourceRange = fragment_range;
        if (mappings.size() == 3) {
            fragment.secondVectorSourceRange = fragment_range;
        }

        state.fragments.push_back(FragmentState{std::move(fragment)});
        state.pendingByLane[lane].push_back(fragment_id);
        for (std::size_t i = 0; i < mappings.size(); ++i) {
            if (end == mappings[i][positions[i]].originalRange.end()) {
                ++positions[i];
            }
        }
        offset = end;
    }

    for (std::size_t i = 0; i < mappings.size(); ++i) {
        panic_if(positions[i] != mappings[i].size(),
                 "Operand mapping extends beyond the task range");
    }
    state.unfinishedFragments = state.fragments.size();
    return state;
}

TransferResult
TaskDistributor::acceptTask(const ArithmeticTask &task)
{
    if (active) {
        return TransferResult::Retry;
    }

    validateTask(task);
    active = makeDistribution(task);
    wakeup();
    return TransferResult::Accepted;
}

bool
TaskDistributor::hasPending() const
{
    return active &&
           std::any_of(active->pendingByLane.begin(),
                       active->pendingByLane.end(),
                       [](const auto &queue) { return !queue.empty(); });
}

void
TaskDistributor::wakeup()
{
    if (!active || progressEvent.scheduled()) {
        return;
    }
    if (active->unfinishedFragments != 0 && !hasPending()) {
        return;
    }
    owner.schedule(progressEvent, owner.clockEdge(Cycles(1)));
}

void
TaskDistributor::evaluate()
{
    if (!active) {
        return;
    }
    if (active->unfinishedFragments == 0) {
        finish();
        return;
    }

    // Como máximo un intento por lane en esta evaluación funcional.
    for (auto &pending : active->pendingByLane) {
        if (pending.empty()) {
            continue;
        }

        auto &fragment = active->fragments[pending.front()];
        panic_if(fragment.phase != FragmentPhase::Pending,
                 "Non-pending fragment in the lane send queue");

        // Reservar recepción antes del callback: puede completar en él.
        fragment.phase = FragmentPhase::Sending;
        switch (sendLane(fragment.task)) {
            case TransferResult::Accepted:
                if (fragment.phase == FragmentPhase::Sending) {
                    fragment.phase = FragmentPhase::Accepted;
                }
                pending.pop_front();
                break;

            case TransferResult::Retry:
                panic_if(fragment.phase != FragmentPhase::Sending,
                         "A lane completed a fragment that it rejected");
                fragment.phase = FragmentPhase::Pending;
                break;

            default:
                panic("Invalid lane transfer result");
        }
    }

    // recvCompletion sólo registra estado; el cierre ocurre en otro evento.
    wakeup();
}

void
TaskDistributor::recvCompletion(const LaneCompletion &completion)
{
    panic_if(!active || !completion.key.valid(),
             "Lane completion without an active task or valid identity");
    panic_if(completion.key.task != active->task.task.key ||
                 completion.key.fragmentId >= active->fragments.size(),
             "Lane completion does not match an active fragment");

    auto &fragment = active->fragments[completion.key.fragmentId];
    panic_if(completion.key != fragment.task.key ||
                 (fragment.phase != FragmentPhase::Sending &&
                  fragment.phase != FragmentPhase::Accepted),
             "Duplicate lane completion or fragment not issued");
    panic_if(completion.status != UnitCompletionStatus::Success,
             "Arithmetic lane reported an invalid completion status");
    panic_if(active->unfinishedFragments == 0,
             "Lane fragment completion accounting underflow");

    fragment.phase = FragmentPhase::Completed;
    --active->unfinishedFragments;
    wakeup();
}

void
TaskDistributor::finish()
{
    panic_if(!active || active->unfinishedFragments != 0 || hasPending(),
             "Cannot complete an unfinished arithmetic task");
    const UnitCompletion completion{
        active->task.task.key, UnitCompletionStatus::Success, std::nullopt};

    // Liberar antes del callback permite admitir la siguiente tarea.
    active.reset();
    sendCompletion(completion);
}

} // namespace gem5::vector_engine
