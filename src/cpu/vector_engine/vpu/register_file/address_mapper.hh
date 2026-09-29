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

#ifndef __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_ADDRESS_MAPPER_HH__
#define __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_ADDRESS_MAPPER_HH__

#include "cpu/vector_engine/common/vrf_types.hh"

namespace gem5::vector_engine
{

/**
 * Servicio compartido de mapeo, sin estado de ejecución, colas ni latencia.
 * El propietario conserva la geometría inmutable y sobrevive al mapper.
 * Sólo comprueba límites físicos; el productor valida LMUL y rango activo.
 */
class AddressMapper
{
  public:
    explicit AddressMapper(const VrfGeometry &geometry);
    AddressMapper(VrfGeometry &&) = delete;

    const VrfGeometry &
    geometry() const
    {
        return vrfGeometry;
    }

    VrfMapping map(const VectorRegRef &reg, const ByteRange &range,
                   const ByteEnable &byte_enable) const;

    // Capacidad uniforme por banco para los 32 registros arquitectónicos.
    // El último nivel de filas puede contener posiciones sin mapeo válido.
    uint64_t rowsPerBank() const;

  private:
    const VrfGeometry &vrfGeometry;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_ADDRESS_MAPPER_HH__
