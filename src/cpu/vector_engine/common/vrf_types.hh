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

#ifndef __CPU_VECTOR_ENGINE_COMMON_VRF_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_VRF_TYPES_HH__

#include <cstdint>
#include <vector>

#include "cpu/vector_engine/common/vector_reg_ref.hh"

namespace gem5::vector_engine
{

using LaneId = uint32_t;
using BankId = uint32_t;

// Una entrada por byte, con valor 0 o 1; no es una máscara de predicación RVV.
using ByteEnable = std::vector<uint8_t>;

/** Geometría compartida e inmutable durante la vida de los módulos del VRF. */
struct VrfGeometry
{
    uint32_t vlenBytes = 0;
    uint32_t laneWordBytes = 0;
    uint32_t numLanes = 0;
    uint32_t banksPerLane = 0;
};

/** Una parte contigua del rango original, contenida en una palabra del VRF. */
struct MappedVrfFragment
{
    LaneId laneId = 0;
    BankId bankId = 0;
    uint64_t row = 0;
    uint32_t byteOffsetInWord = 0;
    ByteRange originalRange;
    ByteEnable wordByteEnable;
};

// En orden de originalRange.offset, incluidos los bytes deshabilitados.
using VrfMapping = std::vector<MappedVrfFragment>;

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VRF_TYPES_HH__
