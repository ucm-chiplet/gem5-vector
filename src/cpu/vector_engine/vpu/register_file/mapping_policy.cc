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

#include "cpu/vector_engine/vpu/register_file/mapping_policy.hh"

#include "base/logging.hh"

namespace gem5::vector_engine
{

namespace
{

/** Reparte palabras entre lanes y, dentro de cada lane, entre sus bancos. */
class WordStripedMapping : public MappingPolicy
{
  public:
    WordLocation
    locate(uint64_t word, const VrfGeometry &geometry) const override
    {
        // Las palabras consecutivas van rotando por las lanes.
        const auto lane = static_cast<LaneId>(word % geometry.numLanes);
        // Después numerarlas dentro de su lane para elegir banco y fila.
        const auto local_word = word / geometry.numLanes;
        const auto bank =
            static_cast<BankId>(local_word % geometry.banksPerLane);
        const auto row = local_word / geometry.banksPerLane;
        return {lane, bank, row};
    }

    uint64_t
    rowsPerBank(const VrfGeometry &geometry) const override
    {
        const uint64_t words_per_lane =
            uint64_t{NumArchitecturalVectorRegs} * geometry.vlenBytes /
            geometry.laneWordBytes / geometry.numLanes;
        const auto banks = geometry.banksPerLane;
        // Si el reparto deja una palabra suelta, hace falta una fila más.
        auto rows = words_per_lane / banks;
        if (words_per_lane % banks != 0) {
            ++rows;
        }
        return rows;
    }
};

} // anonymous namespace

std::unique_ptr<const MappingPolicy>
makeMappingPolicy(MappingPolicyKind kind)
{
    fatal_if(kind != MappingPolicyKind::WordStriped,
             "Only word-striped VRF mapping is implemented");
    return std::make_unique<WordStripedMapping>();
}

} // namespace gem5::vector_engine
