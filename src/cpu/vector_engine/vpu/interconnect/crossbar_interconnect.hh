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

#ifndef __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_CROSSBAR_INTERCONNECT_HH__
#define __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_CROSSBAR_INTERCONNECT_HH__

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "base/statistics.hh"
#include "cpu/vector_engine/vpu/interconnect/vector_interconnect.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Crossbar lógico entre todas las lanes (diseño del hito 2, sección 10.1).
 * No es una NoC de routers ni reproduce el cableado físico de Ara.
 *
 * Recursos y tiempos, en ciclos de la VPU:
 *
 * - Inyección: cada origen acepta como máximo un paquete cada
 *   ceil(bytes / sourceBytesPerCycle) ciclos.
 * - Capacidad por destino: paquetes y bytes aceptados hacia un destino y
 *   aún no entregados, acotados por packetsPerDestination y
 *   bytesPerDestination. El receptor reserva además su propio espacio.
 * - Servicio: cada destino atiende un paquete cada vez, elegido por
 *   round-robin entre los orígenes con paquetes en espera. Ocupa
 *   ceil(bytes / destinationBytesPerCycle) ciclos. Destinos distintos se
 *   atienden en paralelo.
 * - Tránsito: la entrega ocurre transitLatency ciclos después del servicio.
 *   La espera por contención es la que va de la aceptación al servicio.
 *
 * send() devuelve Retry sin efectos si el origen está inyectando, si el
 * destino no tiene capacidad o si el receptor no reserva. Un paquete
 * aceptado se entrega entero una vez, sin fragmentos parciales. Los
 * receptores no pueden llamar a send() desde receive().
 */
class CrossbarInterconnect : public VectorInterconnect
{
  public:
    CrossbarInterconnect(ClockedObject &owner, statistics::Group &stats_parent,
                         const InterconnectParameters &parameters);
    ~CrossbarInterconnect() override;
    CrossbarInterconnect(const CrossbarInterconnect &) = delete;
    CrossbarInterconnect &operator=(const CrossbarInterconnect &) = delete;

    const InterconnectCapabilities &capabilities() const override;
    void bindReceiver(LaneId destination,
                      InterconnectReceiver &receiver) override;
    void setObserver(Observer observer) override;
    TransferResult send(const InterconnectPacket &packet) override;
    void beginDrain() override;
    bool isIdle() const override;

  private:
    struct QueuedPacket
    {
        InterconnectPacket packet;
        Tick acceptedAt = 0;
        // Instante de entrega, fijado al terminar el servicio.
        Tick deliverAt = 0;
    };

    /** Estado de un destino: espera por origen, servicio y tránsito. */
    struct DestinationState
    {
        DestinationState(CrossbarInterconnect &crossbar, LaneId destination,
                         uint32_t endpoints);

        InterconnectReceiver *receiver = nullptr;
        // Paquetes aceptados que esperan servicio, separados por origen
        // para el round-robin. Dentro de un origen se respeta su orden.
        std::vector<std::deque<QueuedPacket>> waiting;
        // Siguiente origen que tendrá prioridad en el round-robin.
        LaneId nextSource = 0;
        std::optional<QueuedPacket> inService;
        // Servidos y en tránsito; la latencia es constante, así que el
        // orden de servicio es también el orden de entrega.
        std::deque<QueuedPacket> inTransit;
        // Ocupación desde la aceptación hasta la entrega.
        uint32_t packets = 0;
        uint32_t bytes = 0;
        EventFunctionWrapper serviceEvent;
        EventFunctionWrapper deliveryEvent;
    };

    statistics::Group stats;
    statistics::Scalar sourceBusyRetries;
    statistics::Scalar destinationFullRetries;
    statistics::Scalar receiverRetries;
    statistics::Scalar contentionWaitTicks;
    statistics::Scalar serviceTicks;
    statistics::Scalar peakDestinationPackets;
    ClockedObject &owner;
    const InterconnectParameters &parameters;
    Observer observer;
    // Primer tick en el que cada origen puede inyectar otro paquete.
    std::vector<Tick> sourceFreeAt;
    std::vector<std::unique_ptr<DestinationState>> destinations;

    Cycles cyclesFor(uint32_t bytes, uint32_t bytes_per_cycle) const;
    void observe(TransportObservation observation,
                 const InterconnectPacket &packet, Tick when) const;
    void startService(LaneId destination);
    void finishService(LaneId destination);
    void deliver(LaneId destination);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_CROSSBAR_INTERCONNECT_HH__
