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

#ifndef __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_VECTOR_INTERCONNECT_HH__
#define __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_VECTOR_INTERCONNECT_HH__

#include <functional>

#include "cpu/vector_engine/common/interconnect_types.hh"

namespace gem5::vector_engine
{

/** Receptor que garantiza espacio antes de que la red acepte un paquete. */
class InterconnectReceiver
{
  public:
    virtual ~InterconnectReceiver() = default;
    // No effects on false. On true reserve key/bytes until delivery.
    virtual bool reserveReception(TransferKey key, uint32_t bytes) = 0;
    // Infallible once reserved. Receiver owns data after this callback.
    virtual void receive(const InterconnectPacket &packet) = 0;
};

/** Topology-independent contract; no active implementation in phase 1.
 * Accepted copies the packet and reserves destination reception. Retry has
 * no effects; the producer keeps the packet and retries next clock cycle.
 * The owner allocates transferId across all sources of a TaskKey.
 * Delivery consumes reserved reception exactly once. It is not a VRF ack.
 * No partial delivery or forwarding between commands is implied.
 */
class VectorInterconnect
{
  public:
    // tick is an absolute simulation tick; payloadBytes counts useful data.
    using Observer = std::function<void(TransportObservation, TransferKey,
                                        Tick, LaneId, LaneId, uint32_t)>;
    virtual ~VectorInterconnect() = default;
    /** Vista inmutable válida durante la vida del transporte. */
    virtual const InterconnectCapabilities &capabilities() const = 0;
    // Bind before traffic. Receiver must outlive the drained transport.
    virtual void bindReceiver(LaneId destination,
                              InterconnectReceiver &receiver) = 0;
    /** Conecta observación antes del tráfico, sin alterar servicio o
     * propiedad. */
    virtual void setObserver(Observer observer) = 0;
    /**
     * Intenta admitir un paquete válido y reservar recepción de forma
     * indivisible: Accepted copia datos; Retry no deja reservas parciales.
     */
    virtual TransferResult send(const InterconnectPacket &packet) = 0;
    // Owner closes new commands. Sends of admitted tasks must still progress.
    virtual void beginDrain() = 0;
    // Includes reception reservations, queued/transit data and events.
    virtual bool isIdle() const = 0;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_VECTOR_INTERCONNECT_HH__
