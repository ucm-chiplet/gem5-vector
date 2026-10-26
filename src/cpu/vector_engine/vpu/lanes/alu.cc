// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/alu.hh"

#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "cpu/vector_engine/common/element_codec.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

Alu::Alu(ClockedObject &owner, LaneId lane_id, ResultSender send_result,
         statistics::Group &stats_parent)
    : stats(&stats_parent, "alu", "integer bundles"),
      elements16(&stats, "elements16", statistics::units::Count::get(),
                 "Admitted 16-bit integer elements"),
      elements32(&stats, "elements32", statistics::units::Count::get(),
                 "Admitted 32-bit integer elements"),
      owner(owner),
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
        ++stats.retries;
        return TransferResult::Retry;
    }
    panic_if(bundle.fpContext || !VectorSupport::supportsIntegerOperation(
                                     bundle.operation, bundle.sewBits),
             "Unsupported operation in an admitted ALU bundle");
    const auto element_bytes = VectorSupport::elementBytes(bundle.sewBits);
    const uint64_t offset =
        uint64_t{bundle.elements.firstElement} * element_bytes;
    const uint64_t size =
        uint64_t{bundle.elements.elementCount} * element_bytes;
    panic_if(!bundle.key.valid() || !bundle.elements.valid() ||
                 !isValid(bundle.emul) ||
                 !bundle.destination.naturallyAligned() ||
                 offset + size > std::numeric_limits<uint32_t>::max() ||
                 bundle.destinationRange.offset != offset ||
                 bundle.destinationRange.size != size ||
                 bundle.lhs.size() != size ||
                 bundle.rhs.size() != size ||
                 (bundle.byteOrder != ByteOrder::little &&
                  bundle.byteOrder != ByteOrder::big),
             "Invalid integer ALU execution bundle");

    active.emplace();
    active->bundle = bundle;
    if (bundle.sewBits == 16) {
        elements16 += bundle.elements.elementCount;
    } else {
        elements32 += bundle.elements.elementCount;
    }
    stats.accept(bundle.elements.elementCount, bundle.destinationRange.size);
    active->result = ExecutionResult{bundle.key,
                                     bundle.sewBits,
                                     bundle.elements,
                                     bundle.destination,
                                     bundle.destinationRange,
                                     ByteBuffer(size),
                                     0,
                                     bundle.byteOrder,
                                     bundle.emul};
    owner.schedule(executeEvent, owner.clockEdge(Cycles(1)));
    return TransferResult::Accepted;
}

void
Alu::execute()
{
    panic_if(!active, "ALU evaluation without an accepted bundle");
    const auto &bundle = active->bundle;
    for (std::size_t i = 0; i < bundle.elements.elementCount; ++i) {
        const auto lhs = ElementCodec::read(bundle.lhs, i, bundle.sewBits,
                                            bundle.byteOrder);
        const auto rhs = ElementCodec::read(bundle.rhs, i, bundle.sewBits,
                                            bundle.byteOrder);
        ElementCodec::write(active->result.values, i, bundle.sewBits,
                            lhs + rhs, bundle.byteOrder);
    }
    auto result = std::move(active->result);
    // Liberar capacidad antes del callback permite una nueva admisión.
    stats.complete();
    active.reset();
    sendResult(result);
}

} // namespace gem5::vector_engine
