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

#ifndef __CPU_VECTOR_ENGINE_COMMON_VPU_PARAMETERS_HH__
#define __CPU_VECTOR_ENGINE_COMMON_VPU_PARAMETERS_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "cpu/vector_engine/common/interconnect_types.hh"
#include "cpu/vector_engine/common/vrf_types.hh"

namespace gem5::vector_engine
{

/** Selección de construcción; sólo existe el reparto actual por palabras. */
enum class MappingPolicyKind : uint8_t
{
    WordStriped
};

/** Bytes por bundle, bundles pendientes y ciclos de latencia/iniciación. */
struct ExecutionParameters
{
    uint32_t widthBytes = 0;
    uint32_t capacity = 1;
    Cycles latency{1};
    Cycles initiationInterval{1};

    /** Reconoce el único perfil funcional implementado, sin temporizarlo. */
    bool
    currentProfile(uint32_t word_bytes) const
    {
        return widthBytes == word_bytes && capacity == 1 &&
               latency == Cycles(1) && initiationInterval == Cycles(1);
    }
};

/**
 * Contextos y buffers por lane/fuente, con máximo de bytes por bundle.
 * El propietario resuelve los anchos antes de entregar esta vista a la lane.
 */
struct LaneParameters
{
    uint32_t taskEntries = 1;
    uint32_t operandEntriesPerSource = 1;
    uint32_t resultEntries = 1;
    uint32_t maxBundleBytes = 0;
    uint32_t alusPerLane = 1;
    ExecutionParameters alu;
    ExecutionParameters fpu;

    /** Evita aceptar capacidades o particiones de FU todavía sin ejecución. */
    bool
    currentProfile(uint32_t word_bytes) const
    {
        return taskEntries == 1 && operandEntriesPerSource == 1 &&
               resultEntries == 1 && maxBundleBytes == word_bytes &&
               alusPerLane == 1 && alu.currentProfile(word_bytes) &&
               fpu.currentProfile(word_bytes);
    }
};

/** Puertos por banco, accesos pendientes y ciclos; no describe el layout. */
struct BankParameters
{
    uint32_t readPorts = 0;
    uint32_t writePorts = 0;
    uint32_t readWritePorts = 1;
    uint32_t accessEntries = 1;
    Cycles latency{1};
    Cycles initiationInterval{1};

    /** El modelo actual sólo sirve un acceso pendiente mediante un puerto RW.
     */
    bool
    currentProfile() const
    {
        return readPorts == 0 && writePorts == 0 && readWritePorts == 1 &&
               accessEntries == 1 && latency == Cycles(1) &&
               initiationInterval == Cycles(1);
    }
};

/** Límite de bytes preparado para bloques; el recorrido activo es por
 * elemento. */
struct VlsuParameters
{
    uint32_t maxBlockBytes = 8;
    uint32_t activeBlocks = 1;
    // Prepared contract only: true is rejected until phase 4.
    bool blockRequests = false;
};

/** Ancho de dirección del objetivo y límite de paquetes físicos en vuelo. */
struct MemoryBackendParameters
{
    unsigned addressBits = 64;
    uint32_t maxOutstandingPackets = 1;
};

/** Perfil de construcción sin red activa; tránsito excluye espera/servicio. */
struct InterconnectParameters
{
    // No topology is implemented in phase 1.
    bool enabled = false;
    InterconnectCapabilities capabilities;
    Cycles transitLatency{1};
};

/** Immutable composition, owned by VectorEngine, outliving all consumers. */
struct VpuParameters
{
    VrfGeometry vrf;
    std::vector<VectorLmul> supportedLmuls;
    std::vector<uint16_t> supportedSews;
    uint32_t commandQueueEntries;
    uint32_t maxActiveCommands = 1;
    uint16_t elenBits = 64;
    LaneParameters lane;
    BankParameters bank;
    VlsuParameters vlsu;
    MemoryBackendParameters memory;
    InterconnectParameters interconnect;
    MappingPolicyKind mapping = MappingPolicyKind::WordStriped;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VPU_PARAMETERS_HH__
