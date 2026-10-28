// Copyright (c) 2026 Félix Garcia Narocki (UCM)
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VRF_PORT_HH__
#define __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VRF_PORT_HH__

#include <functional>
#include <limits>
#include <optional>
#include <variant>

#include "base/logging.hh"
#include "cpu/vector_engine/common/vrf_types.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

// Within one VPU edge, returns precede local progress and VRF arbitration.
// New requests always become eligible on the next edge. These priorities
// therefore do not let callback invocation order change the candidate set.
inline constexpr Event::Priority VpuResponsePriority = -2;
inline constexpr Event::Priority VpuProgressPriority = 1;
inline constexpr Event::Priority VrfArbitrationPriority = 2;

/** Convert a positive VPU-cycle delay without wrapping the tick deadline. */
inline Tick
vpuClockEdge(ClockedObject &owner, Cycles delay)
{
    const Tick edge = owner.clockEdge();
    const auto cycles = static_cast<uint64_t>(delay);
    panic_if(!cycles || cycles > (std::numeric_limits<Tick>::max() - edge) /
                                     owner.clockPeriod(),
             "VPU cycle delay is zero or its tick deadline overflows");
    return edge + cycles * owner.clockPeriod();
}

using VrfRequest = std::variant<VrfReadRequest, VrfWriteRequest>;

inline const VrfAccess &
vrfAccess(const VrfRequest &request)
{
    return std::visit(
        [](const auto &value) -> const VrfAccess & { return value.access; },
        request);
}

inline bool
isVrfWrite(const VrfRequest &request)
{
    return std::holds_alternative<VrfWriteRequest>(request);
}

/**
 * A persistent request owned by a lane context or the active VLSU element.
 * The producer reserves response space before exposing this descriptor and
 * keeps its address, identity and contents stable until granted().
 * eligibleAt is fixed when the request is prepared, at the next VPU edge.
 */
struct VrfOffer
{
    const VrfRequest *request = nullptr;
    Tick eligibleAt = 0;
    Tick publishedAt = 0;
};

/**
 * One statically bound input to a local VRF arbiter, with at most one offer.
 * peek() has no side effects and allocates no request storage in the LRF.
 * An ungranted request remains in its producer; there is no denied FIFO.
 *
 * granted() transfers the operation to the bank and obliges the LRF to
 * return exactly one ReadResponse or WriteAck. It is not a response itself.
 * The LRF copies the granted request and installs return state before this
 * callback. Producers mark their access as waiting before any response.
 * A granted request disappears from peek(), but its producer retains the
 * association and reserved reception until the terminal response arrives.
 *
 * Ports are bound only during construction. Producers, LRF and callbacks
 * survive until all offers, granted operations and responses have drained.
 * This protocol is internal to the VPU and does not use CPU grant tokens.
 */
struct VrfRequestPort
{
    using Peek = std::function<std::optional<VrfOffer>()>;
    using Grant = std::function<void(const VrfAccessKey &)>;

    Peek peek;
    Grant granted;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VRF_PORT_HH__
