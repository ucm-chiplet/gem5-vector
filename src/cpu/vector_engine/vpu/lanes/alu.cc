// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/alu.hh"

#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

Alu::Alu(ClockedObject &owner, LaneId lane_id, ResultSender send_result)
    : owner(owner),
      sendResult(std::move(send_result)),
      executeEvent([this] { execute(); },
                   owner.name() + ".lane" + std::to_string(lane_id) + ".alu")
{
    fatal_if(!sendResult, "ALU requires a result receiver");
}

Alu::~Alu()
{
    panic_if(!isIdle(), "Destroying ALU before its work has drained");
}

TransferResult
Alu::acceptBundle(const ExecutionBundle &bundle)
{
    if (active) {
        return TransferResult::Retry;
    }
    const uint64_t offset = uint64_t{bundle.elements.firstElement} * 4;
    const uint64_t size = uint64_t{bundle.elements.elementCount} * 4;
    panic_if(!bundle.key.valid() || !bundle.elements.valid() ||
                 bundle.operation != ArithmeticOperation::Add ||
                 !bundle.destination.naturallyAligned() ||
                 offset + size > std::numeric_limits<uint32_t>::max() ||
                 bundle.destinationRange.offset != offset ||
                 bundle.destinationRange.size != size ||
                 bundle.lhs.size() != bundle.elements.elementCount ||
                 bundle.rhs.size() != bundle.elements.elementCount,
             "Invalid 32-bit ALU execution bundle");

    active.emplace();
    active->bundle = bundle;
    active->result =
        ExecutionResult{bundle.key, bundle.elements, bundle.destination,
                        bundle.destinationRange,
                        std::vector<uint32_t>(bundle.elements.elementCount)};
    owner.schedule(executeEvent, owner.clockEdge(Cycles(1)));
    return TransferResult::Accepted;
}

void
Alu::execute()
{
    panic_if(!active, "ALU evaluation without an accepted bundle");
    const auto &bundle = active->bundle;
    for (std::size_t i = 0; i < bundle.lhs.size(); ++i) {
        active->result.values[i] = static_cast<uint32_t>(
            uint64_t{bundle.lhs[i]} + uint64_t{bundle.rhs[i]});
    }
    auto result = std::move(active->result);
    // Liberar capacidad antes del callback permite una nueva admisión.
    active.reset();
    sendResult(result);
}

} // namespace gem5::vector_engine
