// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/alu.hh"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "cpu/vector_engine/common/element_codec.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

Alu::Alu(ClockedObject &owner, LaneId lane_id, ResultSender send_result,
         statistics::Group &stats_parent,
         const ExecutionParameters &parameters)
    : stats(&stats_parent, "alu", "integer bundles"),
      elements8(&stats, "elements8", statistics::units::Count::get(),
                "Admitted 8-bit integer elements"),
      elements16(&stats, "elements16", statistics::units::Count::get(),
                 "Admitted 16-bit integer elements"),
      elements32(&stats, "elements32", statistics::units::Count::get(),
                 "Admitted 32-bit integer elements"),
      elements64(&stats, "elements64", statistics::units::Count::get(),
                 "Admitted 64-bit integer elements"),
      capacityStalls(&stats, "capacityStalls", statistics::units::Count::get(),
                     "Bundle attempts blocked by execution capacity"),
      intervalStalls(&stats, "intervalStalls", statistics::units::Count::get(),
                     "Bundle attempts blocked by initiation interval"),
      owner(owner),
      parameters(parameters),
      sendResult(std::move(send_result)),
      executeEvent([this] { execute(); },
                   owner.name() + ".lane" + std::to_string(lane_id) + ".alu",
                   false, VpuResponsePriority)
{
    fatal_if(!parameters.widthBytes ||
                 !parameters.currentProfile(parameters.widthBytes),
             "Invalid timed execution unit profile");
    fatal_if(!sendResult, "ALU requires a result receiver");
    fatal_if(parameters.capacity > pending.max_size(),
             "ALU capacity exceeds host container capacity");
    // Validate configured deadlines before accepting any command effects.
    vpuClockEdge(owner, parameters.latency);
    vpuClockEdge(owner, parameters.initiationInterval);
}

Alu::~Alu()
{
    panic_if(!isIdle(), "Destroying ALU before its work has drained");
}

TransferResult
Alu::acceptBundle(const ExecutionBundle &bundle)
{
    if (pending.size() >= parameters.capacity) {
        ++capacityStalls;
        ++stats.retries;
        return TransferResult::Retry;
    }
    if (curTick() < nextIssue) {
        ++intervalStalls;
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
                 size > parameters.widthBytes || bundle.lhs.size() != size ||
                 bundle.rhs.size() != size ||
                 (bundle.byteOrder != ByteOrder::little &&
                  bundle.byteOrder != ByteOrder::big),
             "Invalid integer ALU execution bundle");
    panic_if(bundle.destination.regCount !=
                 VectorSupport::groupRegisterCount(bundle.emul),
             "ALU destination does not match its effective register group");

    panic_if(std::any_of(pending.begin(), pending.end(),
                         [&](const auto &entry) {
                             return entry.bundle.key == bundle.key;
                         }),
             "Duplicate pending execution bundle");
    const auto ready = vpuClockEdge(owner, parameters.latency);
    nextIssue = vpuClockEdge(owner, parameters.initiationInterval);
    panic_if(ready <= curTick() || nextIssue <= curTick(),
             "Execution timing overflow");
    ExecutionState state;
    state.bundle = bundle;
    state.readyAt = ready;
    switch (bundle.sewBits) {
        case 8:
            elements8 += bundle.elements.elementCount;
            break;
        case 16:
            elements16 += bundle.elements.elementCount;
            break;
        case 32:
            elements32 += bundle.elements.elementCount;
            break;
        case 64:
            elements64 += bundle.elements.elementCount;
            break;
        default:
            panic("Invalid element width in an admitted integer bundle");
    }
    stats.accept(bundle.elements.elementCount, bundle.destinationRange.size);
    // Reservar tantos bytes de resultado como ocupan los elementos recibidos.
    // Conservar identidad y destino para que la lane pueda escribirlos luego.
    state.result = ExecutionResult{bundle.key,
                                   bundle.sewBits,
                                   bundle.elements,
                                   bundle.destination,
                                   bundle.destinationRange,
                                   ByteBuffer(size),
                                   0,
                                   bundle.byteOrder,
                                   bundle.emul};
    pending.push_back(std::move(state));
    if (!responding && !executeEvent.scheduled()) {
        owner.schedule(executeEvent, pending.front().readyAt);
    }
    return TransferResult::Accepted;
}

void
Alu::execute()
{
    responding = true;
    while (!pending.empty() && pending.front().readyAt <= curTick()) {
        auto state = std::move(pending.front());
        pending.pop_front();
        const auto &bundle = state.bundle;
        // Interpretar cada par de elementos usando el orden de bytes del
        // objetivo. uint64_t suma módulo 2^64 sin overflow con signo del host;
        // write conserva los bits bajos para 8/16/32. No se usan shifts por 64
        // bits. RVV 1.0, edición 20240411: Vector Integer Arithmetic
        // Instructions, Vector Single-Width Integer Add and Subtract.
        for (std::size_t i = 0; i < bundle.elements.elementCount; ++i) {
            const auto lhs = ElementCodec::read(bundle.lhs, i, bundle.sewBits,
                                                bundle.byteOrder);
            const auto rhs = ElementCodec::read(bundle.rhs, i, bundle.sewBits,
                                                bundle.byteOrder);
            ElementCodec::write(state.result.values, i, bundle.sewBits,
                                lhs + rhs, bundle.byteOrder);
        }
        auto result = std::move(state.result);
        // Liberar capacidad antes del callback permite una nueva admisión.
        stats.complete();
        sendResult(result);
    }
    responding = false;
    if (!pending.empty() && !executeEvent.scheduled()) {
        owner.schedule(executeEvent, pending.front().readyAt);
    }
}

} // namespace gem5::vector_engine
