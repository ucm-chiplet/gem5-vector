// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/ara_lane.hh"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

AraLane::AraLane(ClockedObject &owner, const AddressMapper &mapper,
                 LaneId lane_id, ByteOrder target_byte_order,
                 ReadSender send_read, WriteSender send_write,
                 CompletionSender send_completion)
    : owner(owner),
      mapper(mapper),
      laneId(lane_id),
      targetByteOrder(target_byte_order),
      sendRead(std::move(send_read)),
      sendWrite(std::move(send_write)),
      sendCompletion(std::move(send_completion)),
      alu(owner, lane_id,
          [this](const ExecutionResult &result) {
              recvExecutionResult(result);
          }),
      fpu(owner, lane_id,
          [this](const ExecutionResult &result) {
              recvExecutionResult(result);
          }),
      progressEvent([this] { evaluate(); },
                    owner.name() + ".lane" + std::to_string(lane_id))
{
    fatal_if(laneId >= mapper.geometry().numLanes,
             "AraLane identity is outside the VRF geometry");
    fatal_if(targetByteOrder != ByteOrder::little &&
                 targetByteOrder != ByteOrder::big,
             "AraLane requires the target byte order");
    fatal_if(!sendRead || !sendWrite || !sendCompletion,
             "AraLane requires VRF and completion endpoints");
}

AraLane::~AraLane()
{
    panic_if(!isIdle(), "Destroying AraLane before its work has drained");
}

void
AraLane::validateTask(const LaneTask &task) const
{
    const auto &arithmetic = task.arithmetic;
    panic_if(!task.key.valid() || task.laneId != laneId ||
                 !task.elements.valid(),
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
                 size > mapper.geometry().laneWordBytes,
             "Lane fragment exceeds one word or its byte range overflows");
    const ByteRange expected{static_cast<uint32_t>(offset),
                             static_cast<uint32_t>(size)};
    panic_if(task.destinationRange != expected ||
                 task.vectorSourceRange != expected ||
                 (second_source && *task.secondVectorSourceRange != expected),
             "Lane byte ranges do not match their elements");

    const ByteEnable byte_enable(expected.size, 1);
    const auto check_register = [&](const VectorRegRef &reg) {
        panic_if(!reg.naturallyAligned() ||
                     (reg.regCount != 1 && reg.regCount != 2 &&
                      reg.regCount != 4 && reg.regCount != 8) ||
                     reg.regCount != arithmetic.destination.regCount,
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
AraLane::makeAccess(const VectorRegRef &reg, const ByteRange &range)
{
    panic_if(nextAccessId == InvalidVrfAccessId,
             "Lane VRF access identifiers exhausted");
    return VrfAccess{VrfAccessKey{active->task.key.task,
                                  VrfRequester{VrfRequesterKind::Lane, laneId},
                                  nextAccessId++},
                     reg, range, ByteEnable(range.size, 1)};
}

TransferResult
AraLane::acceptTask(const LaneTask &task)
{
    if (active) {
        return TransferResult::Retry;
    }
    validateTask(task);
    panic_if(!alu.isIdle() || !fpu.isIdle(),
             "Idle lane still has an active execution unit");
    if (accessTask && *accessTask == task.key.task) {
        // TaskDistributor entrega sus fragmentos en orden creciente por lane.
        panic_if(task.key.fragmentId <= lastFragmentId,
                 "Duplicate or out-of-order lane fragment");
    } else {
        accessTask = task.key.task;
        nextAccessId = 0;
    }
    lastFragmentId = task.key.fragmentId;
    active.emplace();
    auto &state = *active;
    state.task = task;
    // Los dos slots y sus buffers existen antes de emitir cualquier lectura.
    state.bundle =
        ExecutionBundle{task.key,
                        task.arithmetic.operation,
                        task.sewBits,
                        task.elements,
                        task.arithmetic.destination,
                        task.destinationRange,
                        std::vector<uint32_t>(task.elements.elementCount),
                        std::vector<uint32_t>(task.elements.elementCount),
                        task.fpContext};
    state.pending[0] = PendingVrfAccess{
        VrfReadRequest{
            makeAccess(task.arithmetic.vectorSource, task.vectorSourceRange)},
        task.key, AccessRole::Lhs};
    if (const auto *source =
            std::get_if<VectorRegRef>(&task.arithmetic.secondOperand)) {
        state.pending[1] = PendingVrfAccess{
            VrfReadRequest{makeAccess(*source, *task.secondVectorSourceRange)},
            task.key, AccessRole::Rhs};
    } else {
        const auto scalar = static_cast<uint32_t>(std::get<RegVal>(
                                task.arithmetic.secondOperand)) &
                            VectorSupport::elementMask(task.sewBits);
        std::fill(state.bundle.rhs.begin(), state.bundle.rhs.end(), scalar);
        state.rhsReady = true;
    }
    wakeup();
    return TransferResult::Accepted;
}

bool
AraLane::hasReadyWork() const
{
    if (!active || active->phase == Phase::WaitExecution) {
        return false;
    }
    if (active->phase == Phase::Execute || active->phase == Phase::Finish ||
        (active->phase == Phase::Read && active->lhsReady &&
         active->rhsReady)) {
        return true;
    }
    return std::any_of(
        active->pending.begin(), active->pending.end(), [](const auto &entry) {
            return entry && entry->phase == AccessPhase::Prepared;
        });
}

void
AraLane::wakeup()
{
    if (hasReadyWork() && !progressEvent.scheduled()) {
        owner.schedule(progressEvent, owner.clockEdge(Cycles(1)));
    }
}

void
AraLane::sendAccess(AccessRole role)
{
    auto &slot = active->pending[static_cast<std::size_t>(role)];
    if (!slot || slot->phase != AccessPhase::Prepared) {
        return;
    }
    // Copiar el mensaje permite que el callback retire la entrada original.
    const auto request = slot->request;
    slot->phase = AccessPhase::Waiting;
    const auto result = role == AccessRole::Destination
                            ? sendWrite(std::get<VrfWriteRequest>(request))
                            : sendRead(std::get<VrfReadRequest>(request));
    switch (result) {
        case TransferResult::Accepted:
            // No recrear un slot que una respuesta síncrona ya consumió.
            break;
        case TransferResult::Retry:
            panic_if(!slot || slot->phase != AccessPhase::Waiting,
                     "LRF responded to a lane access it rejected");
            slot->phase = AccessPhase::Prepared;
            break;
        default:
            panic("Invalid lane VRF transfer result");
    }
}

void
AraLane::evaluate()
{
    if (!active) {
        return;
    }
    auto &state = *active;
    switch (state.phase) {
        case Phase::Read:
            // Cada fuente puede responder antes que la otra, incluso dentro
            // del envío. Ninguna respuesta ejecuta ni escribe recursivamente.
            sendAccess(AccessRole::Lhs);
            sendAccess(AccessRole::Rhs);
            if (state.lhsReady && state.rhsReady) {
                state.phase = Phase::Execute;
            }
            break;
        case Phase::Execute: {
            state.phase = Phase::WaitExecution;
            const auto result = state.task.fpContext
                                    ? fpu.acceptBundle(state.bundle)
                                    : alu.acceptBundle(state.bundle);
            switch (result) {
                case TransferResult::Accepted:
                    break;
                case TransferResult::Retry:
                    panic_if(state.phase != Phase::WaitExecution,
                             "Execution unit responded to a rejected bundle");
                    state.phase = Phase::Execute;
                    break;
                default:
                    panic("Invalid execution unit transfer result");
            }
            break;
        }
        case Phase::Write:
            sendAccess(AccessRole::Destination);
            break;
        case Phase::Finish:
            finish();
            return;
        case Phase::WaitExecution:
            break;
    }
    wakeup();
}

void
AraLane::recvVrfReadResponse(const ReadResponse &response)
{
    panic_if(!active || active->phase != Phase::Read,
             "Unexpected lane VRF read response");
    auto &state = *active;
    for (std::size_t i = 0; i < 2; ++i) {
        auto &slot = state.pending[i];
        if (!slot) {
            continue;
        }
        const auto *request = std::get_if<VrfReadRequest>(&slot->request);
        panic_if(!request, "Lane read slot contains a write request");
        if (request->access.key != response.key) {
            continue;
        }
        panic_if(slot->phase != AccessPhase::Waiting ||
                     slot->fragment != state.task.key ||
                     slot->role != static_cast<AccessRole>(i) ||
                     response.data.size() != request->access.range.size,
                 "Lane VRF read response has wrong phase, owner or size");
        auto &values = i == 0 ? state.bundle.lhs : state.bundle.rhs;
        auto &ready = i == 0 ? state.lhsReady : state.rhsReady;
        panic_if(ready, "Lane operand was already received");
        const auto element_bytes =
            VectorSupport::elementBytes(state.task.sewBits);
        for (std::size_t element = 0; element < values.size(); ++element) {
            uint32_t value = 0;
            for (unsigned byte = 0; byte < element_bytes; ++byte) {
                const unsigned shift =
                    8 * (targetByteOrder == ByteOrder::little
                             ? byte
                             : element_bytes - 1 - byte);
                value |=
                    uint32_t{response.data[element * element_bytes + byte]}
                    << shift;
            }
            values[element] = value;
        }
        ready = true;
        slot.reset();
        wakeup();
        return;
    }
    panic("Unknown or duplicate lane VRF read response");
}

void
AraLane::recvExecutionResult(const ExecutionResult &result)
{
    panic_if(!active || active->phase != Phase::WaitExecution,
             "Unexpected lane execution result");
    auto &state = *active;
    const auto &task = state.task;
    panic_if(result.key != task.key || result.sewBits != task.sewBits ||
                 result.elements != task.elements ||
                 result.destination != task.arithmetic.destination ||
                 result.destinationRange != task.destinationRange ||
                 result.values.size() != task.elements.elementCount ||
                 !validFpFlags(result.fpFlags) ||
                 (!task.fpContext && result.fpFlags != 0) || state.result ||
                 state.pending[0] || state.pending[1] || state.pending[2] ||
                 !state.lhsReady || !state.rhsReady,
             "Lane execution result does not match its pending fragment");
    state.result = result;
    ByteBuffer data(task.destinationRange.size);
    const auto element_bytes = VectorSupport::elementBytes(task.sewBits);
    for (std::size_t element = 0; element < result.values.size(); ++element) {
        for (unsigned byte = 0; byte < element_bytes; ++byte) {
            const unsigned shift = 8 * (targetByteOrder == ByteOrder::little
                                            ? byte
                                            : element_bytes - 1 - byte);
            data[element * element_bytes + byte] =
                static_cast<uint8_t>(result.values[element] >> shift);
        }
    }
    state.pending[2] = PendingVrfAccess{
        VrfWriteRequest{
            makeAccess(task.arithmetic.destination, task.destinationRange),
            std::move(data)},
        task.key, AccessRole::Destination};
    state.phase = Phase::Write;
    wakeup();
}

void
AraLane::recvVrfWriteAck(const WriteAck &ack)
{
    panic_if(!active || active->phase != Phase::Write,
             "Unexpected lane VRF write acknowledgement");
    auto &slot = active->pending[2];
    panic_if(!slot || slot->phase != AccessPhase::Waiting ||
                 slot->fragment != active->task.key ||
                 slot->role != AccessRole::Destination,
             "Lane write acknowledgement has no pending writeback");
    const auto *request = std::get_if<VrfWriteRequest>(&slot->request);
    panic_if(!request || request->access.key != ack.key,
             "Lane write acknowledgement has wrong identity or direction");
    slot.reset();
    active->phase = Phase::Finish;
    wakeup();
}

void
AraLane::finish()
{
    panic_if(!active || active->phase != Phase::Finish || !active->result ||
                 !alu.isIdle() || !fpu.isIdle() ||
                 std::any_of(active->pending.begin(), active->pending.end(),
                             [](const auto &entry) { return bool(entry); }),
             "Cannot finish a lane fragment with pending work");
    const LaneCompletion completion{active->task.key,
                                    UnitCompletionStatus::Success,
                                    active->result->fpFlags};
    // Sólo WriteAck habilita Finish. Se libera la capacidad antes de notificar
    // para que el receptor pueda admitir el siguiente fragmento en el
    // callback.
    active.reset();
    sendCompletion(completion);
}

} // namespace gem5::vector_engine
