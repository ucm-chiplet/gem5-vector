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

#include <algorithm>
#include <array>
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

/**
 * Tiempo de una operación en una unidad, en ciclos del reloj de la VPU.
 * latency cuenta hasta entregar el resultado; initiationInterval, hasta
 * poder admitir el bundle siguiente en la misma unidad. Son parámetros del
 * modelo, no latencias verificadas de Ara.
 */
struct OperationTiming
{
    Cycles latency{1};
    Cycles initiationInterval{1};

    bool
    valid() const
    {
        return latency >= Cycles(1) && initiationInterval >= Cycles(1);
    }
};

/**
 * Recursos comunes de una clase de unidad (grupo de ALUs o FPU) en una lane.
 *
 * widthBytes son los bytes de cada bundle; se mantiene igual a la palabra
 * del VRF. capacity cuenta bundles aceptados sin resultado entregado, por
 * instancia de ALU o en el pipeline de suma/multiplicación de la FPU.
 * operandEntriesPerSource y resultEntries son los buffers que la lane
 * reserva para esta clase: una clase ocupada no retiene los de la otra,
 * como las colas de operandos y resultados propias de cada unidad en Ara.
 */
struct ExecutionParameters
{
    uint32_t widthBytes = 0;
    uint32_t capacity = 1;
    uint32_t operandEntriesPerSource = 1;
    uint32_t resultEntries = 1;

    /** Exige capacidad y buffers positivos y la anchura de una palabra. */
    bool
    currentProfile(uint32_t word_bytes) const
    {
        return word_bytes && widthBytes == word_bytes && capacity &&
               operandEntriesPerSource && resultEntries;
    }
};

/**
 * ALU entera. Suma y multiplicación son segmentadas. División y resto
 * comparten un divisor no segmentado: ocupan la instancia hasta entregar
 * el resultado, de modo que su intervalo de iniciación es su latencia.
 * divideLatency se indexa por SEW: 8, 16, 32 y 64 bits.
 */
struct AluParameters
{
    ExecutionParameters unit;
    OperationTiming add;
    OperationTiming multiply;
    std::array<Cycles, 4> divideLatency{Cycles(1), Cycles(1), Cycles(1),
                                        Cycles(1)};

    bool
    currentProfile(uint32_t word_bytes) const
    {
        return unit.currentProfile(word_bytes) && add.valid() &&
               multiply.valid() &&
               std::all_of(divideLatency.begin(), divideLatency.end(),
                           [](Cycles c) { return c >= Cycles(1); });
    }
};

/**
 * FPU con dos subrecursos. Suma y multiplicación (incluida la suma binary16
 * interna) comparten un pipeline segmentado con unit.capacity entradas. La
 * división usa un divisor no segmentado con un bundle como máximo, de modo
 * que una división larga no bloquea el pipeline de suma/multiplicación.
 */
struct FpuParameters
{
    ExecutionParameters unit;
    OperationTiming addMultiply;
    Cycles divideLatency{1};

    bool
    currentProfile(uint32_t word_bytes) const
    {
        return unit.currentProfile(word_bytes) && addMultiply.valid() &&
               divideLatency >= Cycles(1);
    }
};

/**
 * Espacio de una lane para tareas y sus unidades de cálculo.
 * taskEntries son contextos compartidos por todas las clases de unidad;
 * los buffers de operandos y resultados pertenecen a cada clase.
 * alusPerLane crea instancias de ALU independientes con los mismos
 * parámetros. VectorEngine fija los tamaños antes de construir la lane;
 * ésta sólo los consulta durante la ejecución.
 */
struct LaneParameters
{
    uint32_t taskEntries = 1;
    uint32_t maxBundleBytes = 0;
    uint32_t alusPerLane = 1;
    AluParameters alu;
    FpuParameters fpu;

    /** Comprueba que la lane se puede construir con los recursos actuales. */
    bool
    currentProfile(uint32_t word_bytes) const
    {
        return taskEntries && maxBundleBytes == word_bytes && alusPerLane &&
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

/**
 * Bloques de memoria unit-stride y entradas de la VLSU en cada LRF.
 * maxBlockBytes limita el buffer del único bloque activo; debe contener
 * elementos enteros de cada SEW habilitado. vrfInputsPerLane fija cuántos
 * accesos por palabra puede ofrecer a la vez la VLSU a cada LRF.
 */
struct VlsuParameters
{
    uint32_t maxBlockBytes = 64;
    uint32_t activeBlocks = 1;
    uint32_t vrfInputsPerLane = 1;
};

/** Tamaño de dirección de la CPU y peticiones pendientes hacia memoria. */
struct MemoryBackendParameters
{
    unsigned addressBits = 64;
    uint32_t maxOutstandingPackets = 1;
};

/**
 * Red entre lanes: modelo elegido al construir, límites comunes y tiempo
 * de viaje tras el servicio, en ciclos de la VPU. La latencia no incluye
 * la espera por contención ni el servicio de ceil(bytes / ancho) ciclos.
 */
struct InterconnectParameters
{
    InterconnectModelKind model = InterconnectModelKind::Crossbar;
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
