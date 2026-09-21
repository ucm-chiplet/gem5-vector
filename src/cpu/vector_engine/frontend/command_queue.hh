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

#ifndef __CPU_VECTOR_ENGINE_FRONTEND_COMMAND_QUEUE_HH__
#define __CPU_VECTOR_ENGINE_FRONTEND_COMMAND_QUEUE_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "cpu/vector_engine/interface/cpu_vector_interface.hh"

namespace gem5::vector_engine::detail
{

/**
 * Admisión conserva el token y una copia del comando reservado.
 * Al recibir dispatch, los compara para consumir la reserva correcta.
 */
struct CommandReservation
{
    GrantToken token;
    VectorCommand descriptor;
};

/**
 * Estado que el control de admisión y CommandQueue necesitan conservar.
 * AraSequencer consulta el comando en cabeza y lo libera al finalizar.
 * CommandQueue es el único propietario de las reservas y de la FIFO.
 */
struct CommandQueueState
{
    // Admisión busca por reservationId; reservar todavía no acepta el comando.
    std::map<uint64_t, CommandReservation> reservations;

    // CommandQueue conserva el orden; la cabeza activa sigue ocupando plaza.
    std::deque<VectorCommand> commands;

    // Admisión obtiene las claves vivas de los descriptores anteriores.
    // El siguiente ID no puede desbordar ni usar InvalidReservationId.
    uint64_t nextReservationId = 0;

    // Admisión deja de dar reservas nuevas mientras termina lo pendiente.
    bool draining = false;
};

} // namespace gem5::vector_engine::detail

namespace gem5::vector_engine
{

/**
 * Extremo de admisión de la VPU y FIFO de comandos aceptados.
 *
 * requestGrant reserva capacidad; dispatch consume esa reserva y confirma
 * accepted después de guardar una copia del comando. La ocupación suma
 * reservas y entradas FIFO, incluida la cabeza que ejecuta el sequencer.
 *
 * La validación estructural pertenece a CpuVectorInterface. Esta clase
 * comprueba el soporte del baseline, duplicados y capacidad; no decodifica
 * instrucciones ni sustituye el validador de la frontera CPU--VPU.
 *
 * La FIFO conserva el orden de dispatch. El emisor debe despachar en orden
 * y AraSequencer debe mantener una única instrucción activa. Esta clase
 * no ejecuta comandos, emite completed ni programa eventos de progreso.
 */
class CommandQueue : public VpuCommandEndpoint
{
  public:
    // Configuración suministrada por el futuro propietario VectorEngine.
    // supported_lmuls y completion_endpoint deben sobrevivir a la cola;
    // el conjunto de LMUL no puede modificarse durante su vida.
    CommandQueue(
        std::size_t queue_depth,
        uint32_t vlen_bytes,
        const std::vector<VectorLmul> &supported_lmuls,
        CpuCompletionEndpoint &completion_endpoint);

    // Copiar la cola duplicaría reservas y obligaciones de aceptación.
    CommandQueue(const CommandQueue &) = delete;
    CommandQueue &operator=(const CommandQueue &) = delete;

    // Precondición: descriptor validado por CpuVectorInterface.
    GrantResult requestGrant(const VectorCommand &command) override;

    // Un token válido garantiza espacio, incluso durante drain.
    void dispatch(
        const GrantToken &token,
        const VectorCommand &command) override;

    // Devuelve una copia; consultar no libera capacidad ni inicia ejecución.
    std::optional<VectorCommand> front() const;

    // Sólo lo llama el sequencer al cerrar el comando de cabeza, después
    // de terminar sus tareas, accesos y writebacks pendientes.
    void releaseHead(CommandKey command);

    std::size_t occupancy() const
    {
        return state.commands.size() + state.reservations.size();
    }

    bool full() const
    {
        return occupancy() == queueDepth;
    }

    // FIFO vacía no implica ausencia de reservas pendientes de dispatch.
    bool empty() const
    {
        return state.commands.empty();
    }

    // Describe sólo esta cola; no certifica el drain de toda la VPU.
    bool isIdle() const
    {
        return empty() && state.reservations.empty();
    }

    // Cierra la admisión nueva sin cancelar reservas ni comandos aceptados.
    void beginDrain()
    {
        state.draining = true;
    }

    // Reabre la admisión cuando ya no quedan reservas ni entradas FIFO.
    void endDrain();

  private:
    const std::size_t queueDepth;
    const uint32_t vlenBytes;

    // Vista no propietaria de una configuración inmutable.
    const std::vector<VectorLmul> &supportedLmuls;
    CpuCompletionEndpoint &completionEndpoint;

    detail::CommandQueueState state;

    // Precondición: LMUL válido; el baseline usa elementos de 32 bits.
    uint64_t maxElements(VectorLmul lmul) const;
    bool contains(CommandKey command) const;

    std::optional<RejectionReason> checkSupport(
        const VectorCommand &command) const;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_FRONTEND_COMMAND_QUEUE_HH__
