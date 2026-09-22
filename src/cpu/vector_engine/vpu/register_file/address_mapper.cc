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

#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"

#include <algorithm>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

AddressMapper::AddressMapper(const VrfGeometry &geometry)
    : vrfGeometry(geometry)
{
    fatal_if(!geometry.vlenBytes || !geometry.laneWordBytes ||
                 !geometry.numLanes || !geometry.banksPerLane,
             "VRF geometry requires positive dimensions");
    fatal_if(geometry.laneWordBytes % 4 != 0,
             "VRF words must contain whole 32-bit elements");

    // El producto de dos dimensiones de 32 bits cabe en 64 bits.
    const uint64_t stripe_bytes =
        uint64_t{geometry.laneWordBytes} * geometry.numLanes;
    fatal_if(geometry.vlenBytes % stripe_bytes != 0,
             "VLEN must be a multiple of lane word bytes times lane count");
}

VrfMapping
AddressMapper::map(const VectorRegRef &reg, const ByteRange &range,
                   const ByteEnable &byte_enable) const
{
    panic_if(!reg.valid() || !range.valid(), "Invalid VRF mapping range");

    const auto &geometry = vrfGeometry;
    const uint64_t capacity = uint64_t{reg.regCount} * geometry.vlenBytes;
    panic_if(uint64_t{range.offset} + range.size > capacity,
             "VRF mapping exceeds its architectural register group");
    panic_if(byte_enable.size() != range.size,
             "VRF byte enable size does not match the range");
    panic_if(std::any_of(byte_enable.begin(), byte_enable.end(),
                         [](uint8_t enable) { return enable > 1; }),
             "VRF byte enables must be zero or one");

    // firstReg < 32 y vlenBytes tiene 32 bits: base y dirección no desbordan.
    const uint64_t base = uint64_t{reg.firstReg} * geometry.vlenBytes;
    VrfMapping mapping;
    uint32_t consumed = 0;

    while (consumed < range.size) {
        const uint32_t offset = range.offset + consumed;
        const uint64_t absolute_byte = base + offset;
        const uint64_t global_word = absolute_byte / geometry.laneWordBytes;
        const uint64_t local_word = global_word / geometry.numLanes;
        const uint32_t word_offset = absolute_byte % geometry.laneWordBytes;
        const uint32_t size = std::min(range.size - consumed,
                                       geometry.laneWordBytes - word_offset);

        MappedVrfFragment fragment;
        fragment.laneId = global_word % geometry.numLanes;
        fragment.bankId = local_word % geometry.banksPerLane;
        fragment.row = local_word / geometry.banksPerLane;
        fragment.byteOffsetInWord = word_offset;
        fragment.originalRange = ByteRange{offset, size};
        fragment.wordByteEnable.assign(geometry.laneWordBytes, 0);
        std::copy_n(byte_enable.begin() + consumed, size,
                    fragment.wordByteEnable.begin() + word_offset);

        mapping.push_back(std::move(fragment));
        consumed += size;
    }

    return mapping;
}

} // namespace gem5::vector_engine
