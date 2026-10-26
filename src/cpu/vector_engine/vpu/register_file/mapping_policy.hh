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

#ifndef __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_MAPPING_POLICY_HH__
#define __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_MAPPING_POLICY_HH__

#include <memory>

#include "cpu/vector_engine/common/vpu_parameters.hh"

namespace gem5::vector_engine
{

/** Ubicación de una palabra completa, sin offsets internos de bytes. */
struct WordLocation
{
    LaneId lane;
    BankId bank;
    uint64_t row;
};

/** Pure placement policy. It does not interpret SEW or architectural state. */
class MappingPolicy
{
  public:
    virtual ~MappingPolicy() = default;
    /** Mapea un índice global de palabra sobre geometría validada e inmutable.
     */
    virtual WordLocation locate(uint64_t word,
                                const VrfGeometry &geometry) const = 0;
    /** Filas reservadas por banco, incluidas posiciones finales sin mapeo. */
    virtual uint64_t rowsPerBank(const VrfGeometry &geometry) const = 0;
};

/** Crea una política propiedad del mapper; falla para modelos inexistentes. */
std::unique_ptr<const MappingPolicy> makeMappingPolicy(MappingPolicyKind kind);

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_MAPPING_POLICY_HH__
