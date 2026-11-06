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

#include "cpu/vector_engine/vpu/interconnect/crossbar_interconnect.hh"

#include <string>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

CrossbarInterconnect::DestinationState::DestinationState(
    CrossbarInterconnect &crossbar, LaneId destination, uint32_t endpoints)
    : waiting(endpoints),
      serviceEvent(
          [&crossbar, destination] { crossbar.finishService(destination); },
          crossbar.owner.name() + ".crossbar.service" +
              std::to_string(destination)),
      deliveryEvent(
          [&crossbar, destination] { crossbar.deliver(destination); },
          crossbar.owner.name() + ".crossbar.delivery" +
              std::to_string(destination))
{}

CrossbarInterconnect::CrossbarInterconnect(
    ClockedObject &owner, statistics::Group &stats_parent,
    const InterconnectParameters &parameters)
    : stats(&stats_parent, "crossbar"),
      sourceBusyRetries(&stats, "sourceBusyRetries",
                        statistics::units::Count::get(),
                        "Attempts rejected while the source was injecting"),
      destinationFullRetries(&stats, "destinationFullRetries",
                             statistics::units::Count::get(),
                             "Attempts rejected by destination packet or "
                             "byte capacity"),
      receiverRetries(&stats, "receiverRetries",
                      statistics::units::Count::get(),
                      "Attempts rejected by receiver reservation"),
      contentionWaitTicks(&stats, "contentionWaitTicks",
                          statistics::units::Tick::get(),
                          "Ticks from acceptance to start of destination "
                          "service"),
      serviceTicks(&stats, "serviceTicks", statistics::units::Tick::get(),
                   "Ticks of destination service, ceil(bytes / width) "
                   "cycles per packet"),
      peakDestinationPackets(&stats, "peakDestinationPackets",
                             statistics::units::Count::get(),
                             "Maximum undelivered packets toward one "
                             "destination"),
      owner(owner),
      parameters(parameters)
{
    const auto &caps = parameters.capabilities;
    fatal_if(!caps.endpoints || !caps.maxPacketBytes ||
                 !caps.packetsPerDestination ||
                 caps.bytesPerDestination < caps.maxPacketBytes ||
                 !caps.sourceBytesPerCycle || !caps.destinationBytesPerCycle,
             "Invalid crossbar capacity contract");
    sourceFreeAt.assign(caps.endpoints, 0);
    destinations.reserve(caps.endpoints);
    for (LaneId lane = 0; lane < caps.endpoints; ++lane) {
        destinations.push_back(
            std::make_unique<DestinationState>(*this, lane, caps.endpoints));
    }
}

CrossbarInterconnect::~CrossbarInterconnect()
{
    // Destruir la red no sustituye drain: no puede quedar un paquete
    // aceptado sin entregar ni un evento que lo referencie.
    panic_if(!isIdle(), "Destroying the crossbar before its work drained");
}

const InterconnectCapabilities &
CrossbarInterconnect::capabilities() const
{
    return parameters.capabilities;
}

void
CrossbarInterconnect::bindReceiver(LaneId destination,
                                   InterconnectReceiver &receiver)
{
    panic_if(destination >= destinations.size() ||
                 destinations[destination]->receiver,
             "Invalid or repeated crossbar receiver binding");
    destinations[destination]->receiver = &receiver;
}

void
CrossbarInterconnect::setObserver(Observer new_observer)
{
    observer = std::move(new_observer);
}

Cycles
CrossbarInterconnect::cyclesFor(uint32_t bytes, uint32_t bytes_per_cycle) const
{
    // ceil(bytes / ancho), al menos un ciclo para un paquete no vacío.
    uint32_t cycles = bytes / bytes_per_cycle;
    if (bytes % bytes_per_cycle != 0 || cycles == 0) {
        ++cycles;
    }
    return Cycles(cycles);
}

void
CrossbarInterconnect::observe(TransportObservation observation,
                              const InterconnectPacket &packet,
                              Tick when) const
{
    if (observer) {
        observer(observation, packet.key, when, packet.sourceLane,
                 packet.destinationLane,
                 static_cast<uint32_t>(packet.data.size()));
    }
}

TransferResult
CrossbarInterconnect::send(const InterconnectPacket &packet)
{
    const auto &caps = parameters.capabilities;
    // Un paquete inválido es un error del productor, no contrapresión.
    panic_if(!packet.valid(caps), "Invalid crossbar packet");
    auto &destination = *destinations[packet.destinationLane];
    panic_if(!destination.receiver, "Crossbar destination has no receiver");
    observe(TransportObservation::Attempt, packet, curTick());

    // Comprobar todos los recursos antes de pedir la reserva al receptor:
    // un Retry no puede dejar ninguna reserva hecha.
    const auto bytes = static_cast<uint32_t>(packet.data.size());
    const auto source = packet.sourceLane;
    if (sourceFreeAt[source] > curTick()) {
        ++sourceBusyRetries;
        return TransferResult::Retry;
    }
    if (destination.packets >= caps.packetsPerDestination ||
        bytes > caps.bytesPerDestination - destination.bytes) {
        ++destinationFullRetries;
        return TransferResult::Retry;
    }
    if (!destination.receiver->reserveReception(packet.key, bytes)) {
        ++receiverRetries;
        return TransferResult::Retry;
    }

    // Aceptado: la red conserva su copia hasta una única entrega.
    sourceFreeAt[source] =
        owner.clockEdge(cyclesFor(bytes, caps.sourceBytesPerCycle));
    ++destination.packets;
    destination.bytes += bytes;
    if (destination.packets > peakDestinationPackets.value()) {
        peakDestinationPackets = destination.packets;
    }
    destination.waiting[source].push_back({packet, curTick(), 0});
    observe(TransportObservation::Accepted, packet, curTick());
    if (!destination.inService) {
        startService(packet.destinationLane);
    }
    return TransferResult::Accepted;
}

void
CrossbarInterconnect::startService(LaneId destination_id)
{
    auto &destination = *destinations[destination_id];
    panic_if(destination.inService, "Crossbar destination already serving");
    const auto endpoints = static_cast<LaneId>(destination.waiting.size());
    // Round-robin: empezar por el origen siguiente al último atendido.
    for (LaneId offset = 0; offset < endpoints; ++offset) {
        const LaneId source = (destination.nextSource + offset) % endpoints;
        auto &queue = destination.waiting[source];
        if (queue.empty()) {
            continue;
        }
        destination.inService = std::move(queue.front());
        queue.pop_front();
        destination.nextSource = (source + 1) % endpoints;
        contentionWaitTicks += curTick() - destination.inService->acceptedAt;
        const auto bytes =
            static_cast<uint32_t>(destination.inService->packet.data.size());
        const Tick end = owner.clockEdge(cyclesFor(
            bytes, parameters.capabilities.destinationBytesPerCycle));
        serviceTicks += end - curTick();
        owner.schedule(destination.serviceEvent, end);
        return;
    }
}

void
CrossbarInterconnect::finishService(LaneId destination_id)
{
    auto &destination = *destinations[destination_id];
    panic_if(!destination.inService, "Crossbar service without a packet");
    // Tras el servicio completo, el paquete viaja entero hacia el receptor.
    auto packet = std::move(*destination.inService);
    destination.inService.reset();
    packet.deliverAt = owner.clockEdge(parameters.transitLatency);
    destination.inTransit.push_back(std::move(packet));
    if (!destination.deliveryEvent.scheduled()) {
        owner.schedule(destination.deliveryEvent,
                       destination.inTransit.front().deliverAt);
    }
    // El siguiente paquete puede empezar su servicio en este mismo flanco.
    startService(destination_id);
}

void
CrossbarInterconnect::deliver(LaneId destination_id)
{
    auto &destination = *destinations[destination_id];
    panic_if(destination.inTransit.empty() ||
                 destination.inTransit.front().deliverAt != curTick(),
             "Crossbar delivery without a packet due now");
    // Liberar la capacidad antes del callback: el receptor ya tiene su
    // espacio reservado y desde ahora es dueño de los datos.
    auto packet = std::move(destination.inTransit.front().packet);
    destination.inTransit.pop_front();
    --destination.packets;
    destination.bytes -= static_cast<uint32_t>(packet.data.size());
    if (!destination.inTransit.empty()) {
        owner.schedule(destination.deliveryEvent,
                       destination.inTransit.front().deliverAt);
    }
    observe(TransportObservation::Delivered, packet, curTick());
    destination.receiver->receive(packet);
}

void
CrossbarInterconnect::beginDrain()
{
    // Los envíos de tareas ya admitidas deben poder terminar: drain no
    // rechaza paquetes. El propietario deja de admitir comandos nuevos.
}

bool
CrossbarInterconnect::isIdle() const
{
    for (const auto &destination : destinations) {
        if (destination->packets || destination->inService ||
            destination->serviceEvent.scheduled() ||
            destination->deliveryEvent.scheduled()) {
            return false;
        }
    }
    return true;
}

} // namespace gem5::vector_engine
