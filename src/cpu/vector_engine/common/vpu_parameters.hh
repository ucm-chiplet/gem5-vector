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

/** Forma de repartir palabras entre lanes y bancos al construir la VPU. */
enum class MappingPolicyKind : uint8_t
{
    WordStriped
};

/** Tamaño del trabajo que recibe una ALU/FPU, capacidad y tiempos en ciclos.
 */
struct ExecutionParameters
{
    uint32_t widthBytes = 0;
    uint32_t capacity = 1;
    Cycles latency{1};
    Cycles initiationInterval{1};

    /** Perfil temporizado; las anchuras independientes quedan para fase 6. */
    bool
    currentProfile(uint32_t word_bytes) const
    {
        return word_bytes && widthBytes == word_bytes && capacity &&
               latency >= Cycles(1) && initiationInterval >= Cycles(1);
    }
};

/**
 * Espacio de una lane para tareas, operandos y resultados, y sus ALU/FPU.
 * VectorEngine fija los tamaños antes de construir la lane; ésta sólo los
 * consulta durante la ejecución.
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

    /** Comprueba que la lane se puede construir con los recursos actuales. */
    bool
    currentProfile(uint32_t word_bytes) const
    {
        return taskEntries && operandEntriesPerSource && resultEntries &&
               maxBundleBytes == word_bytes && alusPerLane == 1 &&
               alu.currentProfile(word_bytes) &&
               fpu.currentProfile(word_bytes);
    }
};

/** Cuántos accesos puede atender un banco y cuánto tarda en responder. */
struct BankParameters
{
    uint32_t readPorts = 0;
    uint32_t writePorts = 0;
    uint32_t readWritePorts = 1;
    uint32_t accessEntries = 1;
    Cycles latency{1};
    Cycles initiationInterval{1};

    /** Exige capacidad, tiempos positivos y servicio de lectura/escritura. */
    bool
    currentProfile() const
    {
        return (readPorts || readWritePorts) &&
               (writePorts || readWritePorts) && accessEntries &&
               latency >= Cycles(1) && initiationInterval >= Cycles(1);
    }
};

/** Tamaño máximo de petición; hoy se envía un solo elemento cada vez. */
struct VlsuParameters
{
    uint32_t maxBlockBytes = 8;
    uint32_t activeBlocks = 1;
    // Agrupar elementos en una petición queda pendiente de la fase 4.
    bool blockRequests = false;
};

/** Tamaño de dirección de la CPU y peticiones pendientes hacia memoria. */
struct MemoryBackendParameters
{
    unsigned addressBits = 64;
    uint32_t maxOutstandingPackets = 1;
};

/** Límites de la futura red y tiempo de viaje una vez atendido el envío. */
struct InterconnectParameters
{
    // La fase 1 declara estos valores, pero todavía no construye una red.
    bool enabled = false;
    InterconnectCapabilities capabilities;
    Cycles transitLatency{1};
};

/**
 * Configuración que VectorEngine conserva durante toda la simulación.
 * Cada módulo recibe su parte por referencia y no la modifica.
 */
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
