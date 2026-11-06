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

#ifndef __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_INTERCONNECT_STATS_HH__
#define __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_INTERCONNECT_STATS_HH__

#include <cstdint>
#include <unordered_map>

#include "base/statistics.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/vpu/interconnect/vector_interconnect.hh"

namespace gem5::vector_engine
{

/**
 * Métricas comunes a cualquier topología, alimentadas sólo por el Observer
 * de VectorInterconnect. Usan las mismas unidades y puntos de medida para
 * que otra red pueda compararse sobre la misma carga:
 *
 * - accepted, completed, bytes, outstanding, peakOutstanding, busyTicks y
 *   workTicks (heredadas de ModuleStats) cuentan paquetes aceptados,
 *   entregados, sus bytes útiles y su ocupación desde la aceptación hasta
 *   la entrega; retries cuenta intentos rechazados.
 * - admissionWaitTicks va del primer intento a la aceptación de cada clave.
 * - transportTicks va de la aceptación a la entrega (latencia observada).
 * - linkPackets y linkBytes son matrices origen x destino de paquetes y
 *   bytes entregados. local* y remote* separan origen igual o distinto
 *   del destino.
 *
 * Una topología puede añadir métricas propias (saltos, conflictos), pero
 * no cambia el significado de éstas. Sólo observa: no programa eventos ni
 * cambia decisiones de admisión.
 */
class InterconnectStats : public ModuleStats
{
  public:
    InterconnectStats(statistics::Group *parent, uint32_t endpoints);

    /** Devuelve un observador que registra en este grupo. */
    VectorInterconnect::Observer observer();

    void record(TransportObservation observation, TransferKey key, Tick when,
                LaneId source, LaneId destination, uint32_t bytes);

  private:
    statistics::Scalar attempts;
    statistics::Scalar admissionWaitTicks;
    statistics::Scalar transportTicks;
    statistics::Scalar deliveredBytes;
    statistics::Scalar localPackets;
    statistics::Scalar remotePackets;
    statistics::Scalar localBytes;
    statistics::Scalar remoteBytes;
    statistics::Vector2d linkPackets;
    statistics::Vector2d linkBytes;
    const uint32_t endpoints;

    // Primer intento de cada clave aún no aceptada y aceptación de cada
    // clave aún no entregada. La clave completa identifica el paquete.
    std::unordered_map<TransferKey, Tick, TransferKeyHash> firstAttempt;
    std::unordered_map<TransferKey, Tick, TransferKeyHash> acceptedAt;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_INTERCONNECT_STATS_HH__
