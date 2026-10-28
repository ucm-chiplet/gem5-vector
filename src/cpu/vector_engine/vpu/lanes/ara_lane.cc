// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/ara_lane.hh"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "cpu/vector_engine/common/element_codec.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

AraLane::AraLane(ClockedObject &owner, const AddressMapper &mapper,
                 const LaneParameters &parameters, LaneId lane_id,
                 ByteOrder target_byte_order, VrfWakeup wake_vrf,
                 CompletionSender send_completion)
    : stats(&owner, "lane" + std::to_string(lane_id), "lane fragments"),
      lhsBuffers(&stats, "lhsBuffers", "reserved LHS buffers"),
      rhsBuffers(&stats, "rhsBuffers", "reserved RHS buffers"),
      resultBuffers(&stats, "resultBuffers", "reserved result buffers"),
      writebacks(&stats, "writebacks", "retained writebacks"),
      vrfReads(&stats, "vrfReads", statistics::units::Count::get(),
               "Operand reads granted by local VRF"),
      vrfWrites(&stats, "vrfWrites", statistics::units::Count::get(),
                "Writebacks granted by local VRF"),
      operandWaitCycles(&stats, "operandWaitCycles",
                        statistics::units::Cycle::get(),
                        "Fragment-cycles waiting for operand buffer pairs"),
      resultWaitCycles(&stats, "resultWaitCycles",
                       statistics::units::Cycle::get(),
                       "FIFO issue head cycles waiting for result space"),
      executionRetries(&stats, "executionRetries",
                       statistics::units::Count::get(),
                       "Bundle attempts rejected by execution unit"),
      owner(owner),
      mapper(mapper),
      parameters(parameters),
      laneId(lane_id),
      targetByteOrder(target_byte_order),
      wakeVrf(std::move(wake_vrf)),
      sendCompletion(std::move(send_completion)),
      alu(
          owner, lane_id,
          [this](const ExecutionResult &result) {
              recvExecutionResult(result);
          },
          stats, parameters.alu),
      fpu(
          owner, lane_id,
          [this](const ExecutionResult &result) {
              recvExecutionResult(result);
          },
          stats, parameters.fpu),
      progressEvent([this] { evaluate(); },
                    owner.name() + ".lane" + std::to_string(lane_id), false,
                    VpuProgressPriority)
{
    fatal_if(!parameters.currentProfile(mapper.geometry().laneWordBytes),
             "Lane configuration requires unimplemented resources");
    fatal_if(laneId >= mapper.geometry().numLanes,
             "AraLane identity is outside the VRF geometry");
    fatal_if(targetByteOrder != ByteOrder::little &&
                 targetByteOrder != ByteOrder::big,
             "AraLane requires the target byte order");
    fatal_if(!wakeVrf || !sendCompletion,
             "AraLane requires VRF wakeup and completion endpoints");
    fatal_if(parameters.taskEntries > contexts.max_size() ||
                 uint64_t{parameters.taskEntries} * 3 >
                     std::vector<VrfRequestPort>().max_size(),
             "Lane context or port count exceeds host container capacity");
    contexts.resize(parameters.taskEntries);
}

AraLane::~AraLane()
{
    panic_if(!isIdle(), "Destroying AraLane before its work has drained");
}

bool
AraLane::isIdle() const
{
    return std::none_of(contexts.begin(), contexts.end(),
                        [](const auto &state) { return state.has_value(); }) &&
           !operandBundles && !resultBundles && alu.isIdle() && fpu.isIdle() &&
           !progressEvent.scheduled();
}

std::vector<VrfRequestPort>
AraLane::vrfPorts()
{
    std::vector<VrfRequestPort> ports;
    ports.reserve(contexts.size() * 3);
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        for (const auto role :
             {AccessRole::Lhs, AccessRole::Rhs, AccessRole::Destination}) {
            ports.push_back({[this, i, role] { return peekAccess(i, role); },
                             [this, i, role](const VrfAccessKey &key) {
                                 recvVrfGrant(i, role, key);
                             }});
        }
    }
    return ports;
}

std::optional<VrfOffer>
AraLane::peekAccess(std::size_t context, AccessRole role) const
{
    if (!contexts[context]) {
        return std::nullopt;
    }
    const auto &slot =
        contexts[context]->pending[static_cast<std::size_t>(role)];
    if (!slot || slot->granted) {
        return std::nullopt;
    }
    return VrfOffer{&slot->request, slot->eligibleAt, slot->publishedAt};
}

void
AraLane::recvVrfGrant(std::size_t context, AccessRole role,
                      const VrfAccessKey &key)
{
    panic_if(!contexts[context], "VRF grant without a lane context");
    auto &slot = contexts[context]->pending[static_cast<std::size_t>(role)];
    panic_if(!slot || slot->granted || slot->eligibleAt > curTick() ||
                 vrfAccess(slot->request).key != key,
             "Unknown or duplicate lane VRF grant");
    slot->granted = true;
    if (role == AccessRole::Destination) {
        ++vrfWrites;
    } else {
        ++vrfReads;
    }
}

void
AraLane::validateTask(const LaneTask &task) const
{
    const auto &arithmetic = task.arithmetic;
    panic_if(!task.key.valid() || task.laneId != laneId ||
                 !task.elements.valid() || !isValid(task.lmul),
             "Invalid lane fragment identity or elements");
    panic_if(!VectorSupport::supportsExecution(arithmetic, task.sewBits,
                                               task.fpContext),
             "Unsupported operation in an admitted lane fragment");
    const auto *second_source =
        std::get_if<VectorRegRef>(&arithmetic.secondOperand);
    panic_if(task.secondVectorSourceRange.has_value() !=
                 (second_source != nullptr),
             "Lane fragment has inconsistent second operand");

    const auto element_bytes = VectorSupport::elementBytes(task.sewBits);
    const uint64_t offset =
        uint64_t{task.elements.firstElement} * element_bytes;
    const uint64_t size = uint64_t{task.elements.elementCount} * element_bytes;
    panic_if(offset + size > std::numeric_limits<uint32_t>::max() ||
                 size > parameters.maxBundleBytes,
             "Lane fragment exceeds one word or its byte range overflows");
    const ByteRange expected{static_cast<uint32_t>(offset),
                             static_cast<uint32_t>(size)};
    panic_if(task.destinationRange != expected ||
                 task.vectorSourceRange != expected ||
                 (second_source && *task.secondVectorSourceRange != expected),
             "Lane byte ranges do not match their elements");

    // regCount describe el contenedor, no los bytes utilizables de un LMUL
    // fraccional. Revisar ambos límites antes de solicitar cualquier acceso.
    panic_if(offset + size > VectorSupport::groupBytes(
                                 mapper.geometry().vlenBytes, task.lmul),
             "Lane fragment exceeds effective LMUL capacity");
    const ByteEnable byte_enable(expected.size, 1);
    const auto expected_regs = VectorSupport::groupRegisterCount(task.lmul);
    const auto check_register = [&](const VectorRegRef &reg) {
        panic_if(!reg.naturallyAligned() || reg.regCount != expected_regs,
                 "Invalid architectural group in a lane fragment");
        const auto mapping = mapper.map(reg, expected, byte_enable);
        panic_if(mapping.size() != 1 || mapping.front().laneId != laneId ||
                     mapping.front().originalRange != expected,
                 "Lane operand must occupy one local VRF word");
    };
    check_register(arithmetic.destination);
    check_register(arithmetic.vectorSource);
    if (second_source) {
        check_register(*second_source);
    }
}

VrfAccess
AraLane::makeAccess(const TaskKey &task, const VectorRegRef &reg,
                    const ByteRange &range)
{
    panic_if(nextAccessId == InvalidVrfAccessId,
             "Lane VRF access identifiers exhausted");
    return VrfAccess{VrfAccessKey{task,
                                  VrfRequester{VrfRequesterKind::Lane, laneId},
                                  nextAccessId++},
                     reg, range, ByteEnable(range.size, 1)};
}

TransferResult
AraLane::acceptTask(const LaneTask &task)
{
    auto free = std::find_if(contexts.begin(), contexts.end(),
                             [](const auto &state) { return !state; });
    if (free == contexts.end()) {
        ++stats.retries;
        return TransferResult::Retry;
    }
    validateTask(task);
    if (accessTask && *accessTask == task.key.task) {
        panic_if(task.key.fragmentId <= lastFragmentId,
                 "Duplicate or out-of-order lane fragment");
    } else {
        panic_if(std::any_of(contexts.begin(), contexts.end(),
                             [](const auto &state) { return bool(state); }) ||
                     !alu.isIdle() || !fpu.isIdle(),
                 "Lane cannot admit a second task in phase 3");
        accessTask = task.key.task;
        nextAccessId = 0;
    }
    lastFragmentId = task.key.fragmentId;
    free->emplace();
    (*free)->task = task;
    stats.accept(task.elements.elementCount, task.destinationRange.size);
    wakeup();
    return TransferResult::Accepted;
}

std::vector<std::size_t>
AraLane::orderedContexts() const
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (contexts[i]) {
            order.push_back(i);
        }
    }
    // Reused physical slots do not change fragment admission/issue order.
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return contexts[a]->task.key.fragmentId <
               contexts[b]->task.key.fragmentId;
    });
    return order;
}

void
AraLane::prepareOperands(LaneFragmentState &state)
{
    if (operandBundles == parameters.operandEntriesPerSource) {
        ++operandWaitCycles;
        return;
    }
    const auto &task = state.task;
    ++operandBundles;
    state.operandsReserved = true;
    lhsBuffers.accept(task.elements.elementCount, task.destinationRange.size);
    rhsBuffers.accept(task.elements.elementCount, task.destinationRange.size);
    // Pair acquisition prevents two contexts each retaining only one source.
    // A scalar is replicated in its reserved RHS buffer without a VRF read.
    state.bundle = ExecutionBundle{task.key,
                                   task.arithmetic.operation,
                                   task.sewBits,
                                   task.elements,
                                   task.arithmetic.destination,
                                   task.destinationRange,
                                   ByteBuffer(task.destinationRange.size),
                                   ByteBuffer(task.destinationRange.size),
                                   task.fpContext,
                                   targetByteOrder,
                                   task.lmul};
    const auto eligible = vpuClockEdge(owner, Cycles(1));
    state.pending[0] = PendingVrfAccess{
        VrfReadRequest{makeAccess(task.key.task, task.arithmetic.vectorSource,
                                  task.vectorSourceRange)},
        eligible, curTick()};
    if (const auto *source =
            std::get_if<VectorRegRef>(&task.arithmetic.secondOperand)) {
        state.pending[1] = PendingVrfAccess{
            VrfReadRequest{makeAccess(task.key.task, *source,
                                      *task.secondVectorSourceRange)},
            eligible, curTick()};
    } else {
        const auto scalar = std::get<RegVal>(task.arithmetic.secondOperand);
        for (uint32_t i = 0; i < task.elements.elementCount; ++i) {
            ElementCodec::write(state.bundle.rhs, i, task.sewBits, scalar,
                                targetByteOrder);
        }
        state.rhsReady = true;
    }
    state.phase = Phase::Read;
    wakeVrf();
}

void
AraLane::tryExecute(LaneFragmentState &state)
{
    if (!state.resultReserved) {
        if (resultBundles == parameters.resultEntries) {
            ++resultWaitCycles;
            return;
        }
        ++resultBundles;
        state.resultReserved = true;
        resultBuffers.accept(state.task.elements.elementCount,
                             state.task.destinationRange.size);
    }
    // The single reservation covers waiting for FU admission, execution,
    // retained data and writeback. FU capacity is a different resource.
    state.phase = Phase::WaitExecution;
    TransferResult accepted;
    if (state.task.fpContext) {
        accepted = fpu.acceptBundle(state.bundle);
    } else {
        accepted = alu.acceptBundle(state.bundle);
    }
    if (accepted == TransferResult::Retry) {
        panic_if(state.phase != Phase::WaitExecution,
                 "FU responded to a rejected lane bundle");
        ++executionRetries;
        state.phase = Phase::Execute;
        return;
    }
    panic_if(accepted != TransferResult::Accepted,
             "Invalid execution unit transfer result");
    panic_if(!state.operandsReserved || !operandBundles,
             "Missing operand reservation at execution admission");
    state.operandsReserved = false;
    --operandBundles;
    lhsBuffers.complete();
    rhsBuffers.complete();
    // The FU owns its bounded copy until result delivery. Release lane bytes
    // rather than keeping an unaccounted second operand buffer in each slot.
    ByteBuffer().swap(state.bundle.lhs);
    ByteBuffer().swap(state.bundle.rhs);
}

void
AraLane::wakeup()
{
    const bool ready =
        std::any_of(contexts.begin(), contexts.end(), [](const auto &state) {
            return state && (state->phase == Phase::Prepare ||
                             state->phase == Phase::Execute ||
                             state->phase == Phase::Finish);
        });
    if (ready && !progressEvent.scheduled()) {
        owner.schedule(progressEvent, vpuClockEdge(owner, Cycles(1)));
    }
}

void
AraLane::evaluate()
{
    for (const auto i : orderedContexts()) {
        if (contexts[i]->phase == Phase::Finish) {
            finish(i);
        }
    }
    const auto order = orderedContexts();
    for (const auto i : order) {
        if (contexts[i]->phase == Phase::Prepare) {
            prepareOperands(*contexts[i]);
        }
    }
    // Start at most one bundle per edge, in fragment order. A younger ready
    // fragment cannot bypass an older fragment still collecting operands.
    for (const auto i : order) {
        auto &state = *contexts[i];
        if (state.phase == Phase::Prepare || state.phase == Phase::Read) {
            break;
        }
        if (state.phase == Phase::Execute) {
            tryExecute(state);
            break;
        }
    }
    wakeup();
}

void
AraLane::recvVrfReadResponse(const ReadResponse &response)
{
    for (auto &context : contexts) {
        if (!context) {
            continue;
        }
        auto &state = *context;
        for (std::size_t role = 0; role < 2; ++role) {
            auto &slot = state.pending[role];
            if (!slot || vrfAccess(slot->request).key != response.key) {
                continue;
            }
            panic_if(!slot->granted || state.phase != Phase::Read ||
                         !state.operandsReserved ||
                         response.data.size() !=
                             vrfAccess(slot->request).range.size,
                     "Lane operand response has wrong phase or size");
            if (role == 0) {
                panic_if(state.lhsReady, "Lane LHS already received");
                state.bundle.lhs = response.data;
                state.lhsReady = true;
            } else {
                panic_if(state.rhsReady, "Lane RHS already received");
                state.bundle.rhs = response.data;
                state.rhsReady = true;
            }
            slot.reset();
            if (state.lhsReady && state.rhsReady) {
                state.phase = Phase::Execute;
            }
            wakeup();
            return;
        }
    }
    panic("Unknown or duplicate lane VRF read response");
}

void
AraLane::recvExecutionResult(const ExecutionResult &result)
{
    auto found =
        std::find_if(contexts.begin(), contexts.end(), [&](const auto &state) {
            return state && state->task.key == result.key;
        });
    panic_if(found == contexts.end(), "Unknown lane execution result");
    auto &state = **found;
    const auto &task = state.task;
    panic_if(state.phase != Phase::WaitExecution || !state.resultReserved ||
                 state.operandsReserved || result.sewBits != task.sewBits ||
                 result.elements != task.elements ||
                 result.destination != task.arithmetic.destination ||
                 result.destinationRange != task.destinationRange ||
                 result.values.size() != task.destinationRange.size ||
                 result.byteOrder != targetByteOrder ||
                 result.emul != task.lmul || !validFpFlags(result.fpFlags) ||
                 (!task.fpContext && result.fpFlags != 0) || state.result ||
                 state.pending[0] || state.pending[1] || state.pending[2] ||
                 !state.lhsReady || !state.rhsReady,
             "Lane result does not match its reserved fragment");
    // Transfer the retained result bytes into the persistent write request.
    // The metadata keeps flags; one output credit covers both until WriteAck.
    state.result = result;
    state.pending[2] = PendingVrfAccess{
        VrfWriteRequest{makeAccess(task.key.task, task.arithmetic.destination,
                                   task.destinationRange),
                        std::move(state.result->values)},
        vpuClockEdge(owner, Cycles(1)), curTick()};
    state.phase = Phase::Write;
    writebacks.accept(task.elements.elementCount, task.destinationRange.size);
    wakeVrf();
}

void
AraLane::recvVrfWriteAck(const WriteAck &ack)
{
    for (auto &context : contexts) {
        if (!context || !context->pending[2] ||
            vrfAccess(context->pending[2]->request).key != ack.key) {
            continue;
        }
        auto &state = *context;
        panic_if(state.phase != Phase::Write || !state.pending[2]->granted ||
                     !state.resultReserved || !resultBundles,
                 "Lane WriteAck without a granted writeback reservation");
        state.pending[2].reset();
        state.resultReserved = false;
        --resultBundles;
        resultBuffers.complete();
        writebacks.complete();
        state.phase = Phase::Finish;
        wakeup();
        return;
    }
    panic("Unknown or duplicate lane VRF write acknowledgement");
}

void
AraLane::finish(std::size_t context)
{
    auto &state = contexts[context];
    panic_if(!state || state->phase != Phase::Finish || !state->result ||
                 state->operandsReserved || state->resultReserved ||
                 std::any_of(state->pending.begin(), state->pending.end(),
                             [](const auto &entry) { return bool(entry); }),
             "Cannot finish a lane fragment with pending obligations");
    const LaneCompletion completion{state->task.key,
                                    UnitCompletionStatus::Success,
                                    state->result->fpFlags};
    // Other fragments may still occupy the FUs: only this fragment closes.
    stats.complete();
    state.reset();
    sendCompletion(completion);
}

} // namespace gem5::vector_engine
