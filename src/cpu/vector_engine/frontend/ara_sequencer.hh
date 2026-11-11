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

#ifndef __CPU_VECTOR_ENGINE_FRONTEND_ARA_SEQUENCER_HH__
#define __CPU_VECTOR_ENGINE_FRONTEND_ARA_SEQUENCER_HH__

#include <cstdint>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/common/unit_completion.hh"
#include "cpu/vector_engine/frontend/command_queue.hh"
#include "cpu/vector_engine/frontend/vector_scoreboard.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine::detail
{

/**
 * AraSequencer conserva aquí cada comando iniciado y su tarea hasta
 * entregar su finalización. Puede haber hasta max_active_commands.
 */
struct ActiveCommandState
{
    VectorCommand command;

    // AraSequencer crea una tarea para [vstart, vl), salvo si está vacío.
    // Conserva el mismo descriptor y TaskKey cuando la unidad devuelve Retry.
    std::optional<std::variant<ArithmeticTask, MemoryTask, SlideTask>> task;

    // La unidad la ha aceptado, pero no terminado; AraSequencer no la reenvía.
    bool taskAccepted = false;

    // La unidad responde cuando terminan los accesos y las escrituras.
    // AraSequencer comprueba que TaskKey corresponde a la tarea conservada.
    std::optional<UnitCompletion> completion;
};

} // namespace gem5::vector_engine::detail

namespace gem5::vector_engine
{

/**
 * Inicia en orden los comandos de CommandQueue y mantiene varios activos.
 * Recibe comandos ya validados estructuralmente y admitidos por la cola.
 *
 * Sólo la cabeza de pendientes puede iniciar, sin bypass. Antes de hacerlo
 * se comprueban, en este orden de diagnóstico no excluyente:
 *
 * - capacidad: hay menos de max_active_commands activos;
 * - barrera de memoria: una carga o store sólo empieza sin otros activos,
 *   y nada empieza mientras haya una carga o store activa;
 * - aislamiento: un comando con vstart != 0 se ejecuta solo;
 * - dependencias: VectorScoreboard no detecta RAW, WAR ni WAW con los
 *   comandos activos (todos anteriores, porque el inicio es en orden).
 *
 * Al iniciar, el comando reserva sus registros y pasa a iniciado en la
 * cola, que conserva su crédito. Las tareas se envían en orden de inicio;
 * una tarea en Retry retiene el envío de las posteriores. Las unidades
 * pueden terminar desordenadas: cada finalización se asocia por TaskKey y
 * produce un único completed por comando. Al cerrar un comando se libera
 * su reserva del scoreboard, se notifica a la CPU y después se devuelve
 * su crédito a la cola. No hay chaining: la reserva sólo se libera cuando
 * la unidad confirma todas sus escrituras.
 *
 * Se inicia como máximo un comando por ciclo, y el cierre de un comando
 * permite iniciar otro a partir del ciclo siguiente.
 *
 * El propietario conecta los envíos a TaskDistributor, AraVLSU y SlideUnit
 * y llama a wakeup después de dispatch. Una slide no es barrera de
 * memoria: sólo respeta capacidad, aislamiento y dependencias. Las unidades
 * copian la tarea al aceptarla y llaman a recvCompletion sólo después de
 * cerrar accesos y writebacks. Los callbacks pueden responder durante el
 * envío; no destruyen el módulo.
 *
 * El propietario, la cola y los receptores deben sobrevivir al sequencer.
 * La destrucción no sustituye drain ni cancela tareas aceptadas.
 */
class AraSequencer
{
  public:
    using ArithmeticSender =
        std::function<TransferResult(const ArithmeticTask &)>;
    using MemorySender = std::function<TransferResult(const MemoryTask &)>;
    using SlideSender = std::function<TransferResult(const SlideTask &)>;

    AraSequencer(ClockedObject &owner, CommandQueue &queue,
                 CpuCompletionEndpoint &completion_endpoint,
                 uint32_t vlen_bytes, uint32_t max_active_commands,
                 ArithmeticSender send_arithmetic, MemorySender send_memory,
                 SlideSender send_slide);

    ~AraSequencer();

    AraSequencer(const AraSequencer &) = delete;
    AraSequencer &operator=(const AraSequencer &) = delete;

    // Programa progreso, nunca ejecuta el comando directamente.
    void wakeup();

    // La unidad puede responder durante la llamada de envío.
    void recvCompletion(const UnitCompletion &completion);

    // Sólo describe este módulo, no el drain global de la VPU.
    bool
    isIdle() const
    {
        return active.empty() && !progressEvent.scheduled();
    }

  private:
    using BackendTask = std::variant<ArithmeticTask, MemoryTask, SlideTask>;

    /** Causas, no excluyentes, por las que la cabeza no puede iniciar. */
    struct StartBlockers
    {
        bool capacity = false;
        bool memoryBarrier = false;
        bool isolation = false;
        HazardSet hazards;

        bool
        any() const
        {
            return capacity || memoryBarrier || isolation || hazards.any();
        }
    };

    /**
     * Espera de la cabeza actual. Se reparte entre las causas vigentes en
     * cada evaluación; el intervalo se cierra al iniciar o cambiar causas.
     */
    struct HeadWait
    {
        CommandKey command;
        Tick since = 0;
        StartBlockers blockers;
    };

    ModuleStats stats;
    statistics::Scalar arithmeticCommands;
    statistics::Scalar loadCommands;
    statistics::Scalar storeCommands;
    statistics::Scalar slideCommands;
    statistics::Scalar emptyCommands;
    statistics::Scalar overlappedStarts;
    statistics::Scalar multiActiveTicks;
    statistics::Scalar capacityWaitTicks;
    statistics::Scalar memoryBarrierWaitTicks;
    statistics::Scalar isolationWaitTicks;
    statistics::Scalar hazardWaitTicks;
    statistics::Scalar rawWaitTicks;
    statistics::Scalar warWaitTicks;
    statistics::Scalar wawWaitTicks;
    ClockedObject &owner;
    CommandQueue &queue;
    CpuCompletionEndpoint &completionEndpoint;

    // Mismo VLEN inmutable que recibe CommandQueue.
    const uint32_t vlenBytes;
    const uint32_t maxActiveCommands;

    ArithmeticSender sendArithmetic;
    MemorySender sendMemory;
    SlideSender sendSlide;

    // Comandos iniciados, en orden de inicio (de más antiguo a más joven).
    std::vector<detail::ActiveCommandState> active;
    VectorScoreboard scoreboard;
    std::optional<HeadWait> headWait;
    Tick lastOccupancyChange = 0;
    EventFunctionWrapper progressEvent;

    void evaluate();
    BackendTask makeTask(const VectorCommand &command) const;
    static const UnitTask &taskOf(const detail::ActiveCommandState &state);
    static bool isMemory(const VectorCommand &command);
    StartBlockers blockersFor(const VectorCommand &command) const;
    void accountHeadWait(const std::optional<VectorCommand> &head,
                         const StartBlockers &blockers);
    void noteOccupancyChange();
    void start(const VectorCommand &command);
    bool sendPendingTasks();
    bool finishCompleted();
    void finish(std::size_t index);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_FRONTEND_ARA_SEQUENCER_HH__
