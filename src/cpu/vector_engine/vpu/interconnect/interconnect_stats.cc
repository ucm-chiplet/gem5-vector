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

#include "cpu/vector_engine/vpu/interconnect/interconnect_stats.hh"

#include "base/logging.hh"

namespace gem5::vector_engine
{

InterconnectStats::InterconnectStats(statistics::Group *parent,
                                     uint32_t endpoints)
    : ModuleStats(parent, "interconnect", "packets"),
      attempts(this, "attempts", statistics::units::Count::get(),
               "Send attempts, including rejected retries"),
      admissionWaitTicks(this, "admissionWaitTicks",
                         statistics::units::Tick::get(),
                         "Ticks from first attempt to acceptance"),
      transportTicks(this, "transportTicks", statistics::units::Tick::get(),
                     "Ticks from acceptance to delivery"),
      deliveredBytes(this, "deliveredBytes", statistics::units::Byte::get(),
                     "Useful bytes delivered to receivers"),
      localPackets(this, "localPackets", statistics::units::Count::get(),
                   "Delivered packets whose source is their destination"),
      remotePackets(this, "remotePackets", statistics::units::Count::get(),
                    "Delivered packets between different endpoints"),
      localBytes(this, "localBytes", statistics::units::Byte::get(),
                 "Delivered bytes whose source is their destination"),
      remoteBytes(this, "remoteBytes", statistics::units::Byte::get(),
                  "Delivered bytes between different endpoints"),
      linkPackets(this, "linkPackets", statistics::units::Count::get(),
                  "Delivered packets per source x destination endpoint"),
      linkBytes(this, "linkBytes", statistics::units::Byte::get(),
                "Delivered bytes per source x destination endpoint"),
      endpoints(endpoints)
{
    fatal_if(!endpoints, "Interconnect statistics require endpoints");
    linkPackets.init(endpoints, endpoints);
    linkBytes.init(endpoints, endpoints);
}

VectorInterconnect::Observer
InterconnectStats::observer()
{
    return [this](TransportObservation observation, TransferKey key, Tick when,
                  LaneId source, LaneId destination, uint32_t bytes) {
        record(observation, key, when, source, destination, bytes);
    };
}

void
InterconnectStats::record(TransportObservation observation, TransferKey key,
                          Tick when, LaneId source, LaneId destination,
                          uint32_t bytes)
{
    panic_if(!key.valid() || source >= endpoints || destination >= endpoints,
             "Interconnect observation with invalid identity or endpoint");
    switch (observation) {
        case TransportObservation::Attempt: {
            ++attempts;
            // Un intento repetido de una clave aún no aceptada es un retry.
            const auto inserted = firstAttempt.emplace(key, when);
            if (!inserted.second) {
                ++retries;
            }
            break;
        }
        case TransportObservation::Accepted: {
            const auto first = firstAttempt.find(key);
            panic_if(first == firstAttempt.end(),
                     "Interconnect acceptance without an attempt");
            admissionWaitTicks += when - first->second;
            firstAttempt.erase(first);
            panic_if(!acceptedAt.emplace(key, when).second,
                     "Interconnect accepted a key twice");
            accept(0, bytes);
            break;
        }
        case TransportObservation::Delivered: {
            const auto accepted_at = acceptedAt.find(key);
            panic_if(accepted_at == acceptedAt.end(),
                     "Interconnect delivery without acceptance");
            transportTicks += when - accepted_at->second;
            acceptedAt.erase(accepted_at);
            complete();
            deliveredBytes += bytes;
            linkPackets[source][destination] += 1;
            linkBytes[source][destination] += bytes;
            if (source == destination) {
                ++localPackets;
                localBytes += bytes;
            } else {
                ++remotePackets;
                remoteBytes += bytes;
            }
            break;
        }
        default:
            panic("Invalid interconnect observation");
    }
}

} // namespace gem5::vector_engine
