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
#include <memory>

#include "base/statistics.hh"
#include "cpu/vector_engine/common/interconnect_types.hh"
#include "cpu/vector_engine/common/vpu_parameters.hh"
#include "sim/clocked_object.hh"

namespace gem5::vector_engine
{

/** Receptor que garantiza espacio antes de que la red acepte un paquete. */
class InterconnectReceiver
{
  public:
    virtual ~InterconnectReceiver() = default;
    // Devuelve true si guarda espacio para estos bytes hasta su entrega.
    // Si devuelve false, no debe dejar ninguna reserva hecha.
    virtual bool reserveReception(TransferKey key, uint32_t bytes) = 0;
    // Una entrega ya reservada debe poder recibirse sin rechazarla. Desde
    // esta llamada el receptor se encarga de conservar y procesar los datos.
    virtual void receive(const InterconnectPacket &packet) = 0;
};

/**
 * Interfaz común para enviar datos entre lanes, sea cual sea la red elegida.
 * Al aceptar un paquete, la red copia sus datos y el receptor reserva su
 * espacio. Si pide reintentar, no queda ninguna reserva: el emisor conserva
 * el mismo paquete y su TransferKey y vuelve a llamar a send() desde su
 * propio progreso, como pronto en el ciclo siguiente. La red no avisa de
 * capacidad libre, de modo que el emisor no depende de detalles internos.
 *
 * El propietario asigna transferId entre todos los emisores de una tarea.
 * Cada paquete aceptado se entrega entero una sola vez, quizá en otro orden
 * que el de envío; su entrega aún no confirma que el VRF se haya escrito ni
 * habilita pasar resultados entre comandos. La topología, las rutas y el
 * arbitraje quedan ocultos detrás de esta interfaz.
 */
class VectorInterconnect
{
  public:
    // Registra el instante absoluto de simulación y el número de bytes útiles.
    using Observer = std::function<void(TransportObservation, TransferKey,
                                        Tick, LaneId, LaneId, uint32_t)>;
    virtual ~VectorInterconnect() = default;
    /** Devuelve los límites de la red, válidos mientras ésta exista. */
    virtual const InterconnectCapabilities &capabilities() const = 0;
    // Conectar antes de enviar. El receptor debe existir hasta que la red
    // haya terminado de entregar todo su trabajo pendiente.
    virtual void bindReceiver(LaneId destination,
                              InterconnectReceiver &receiver) = 0;
    /** Instala quien registra los envíos antes de que empiece el tráfico. */
    virtual void setObserver(Observer observer) = 0;
    /**
     * Intenta enviar un paquete válido. Accepted significa que se copiaron
     * los datos y se reservó su recepción; Retry permite volver a intentarlo
     * sin tener que deshacer reservas parciales.
     */
    virtual TransferResult send(const InterconnectPacket &packet) = 0;
    // El propietario deja de admitir comandos nuevos. Los envíos de tareas
    // que ya estaban admitidas deben poder continuar hasta terminar.
    virtual void beginDrain() = 0;
    // Sólo está libre cuando no quedan reservas, datos ni eventos pendientes.
    virtual bool isIdle() const = 0;
};

/**
 * Construye el modelo de red elegido. owner aporta reloj y eventos;
 * stats_parent recibe las estadísticas propias del modelo. Los límites ya
 * vienen validados por VectorEngine.
 */
std::unique_ptr<VectorInterconnect>
makeInterconnect(ClockedObject &owner, statistics::Group &stats_parent,
                 const InterconnectParameters &parameters);

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_INTERCONNECT_VECTOR_INTERCONNECT_HH__
