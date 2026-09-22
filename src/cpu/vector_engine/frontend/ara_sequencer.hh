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

#include <functional>
#include <optional>
#include <variant>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/unit_completion.hh"
#include "cpu/vector_engine/frontend/command_queue.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine::detail
{

/**
 * AraSequencer conserva aquí el comando que está ejecutando y su tarea.
 * En la versión inicial sólo puede haber una entrada activa.
 */
struct ActiveCommandState
{
    VectorCommand command;

    // AraSequencer crea una tarea para [vstart, vl), salvo si está vacío.
    // Conserva el mismo descriptor y TaskKey cuando la unidad devuelve Retry.
    std::optional<std::variant<ArithmeticTask, MemoryTask>> task;

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
 * Ejecuta una cabeza FIFO cada vez y conserva su crédito hasta finalizar.
 * Recibe comandos ya validados estructuralmente y admitidos por la cola.
 *
 * El propietario conecta los envíos a TaskDistributor y AraVLSU y llama a
 * wakeup después de dispatch. Las unidades copian la tarea al aceptarla y
 * llaman a recvCompletion sólo después de cerrar accesos y writebacks.
 * Los callbacks pueden responder durante el envío; no destruyen el módulo.
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

    AraSequencer(ClockedObject &owner, CommandQueue &queue,
                 CpuCompletionEndpoint &completion_endpoint,
                 uint32_t vlen_bytes, ArithmeticSender send_arithmetic,
                 MemorySender send_memory);

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
        return !active && !progressEvent.scheduled();
    }

  private:
    using BackendTask = std::variant<ArithmeticTask, MemoryTask>;

    ClockedObject &owner;
    CommandQueue &queue;
    CpuCompletionEndpoint &completionEndpoint;

    // Mismo VLEN inmutable que recibe CommandQueue.
    const uint32_t vlenBytes;

    ArithmeticSender sendArithmetic;
    MemorySender sendMemory;

    std::optional<detail::ActiveCommandState> active;
    EventFunctionWrapper progressEvent;

    void evaluate();
    BackendTask makeTask(const VectorCommand &command) const;
    const UnitTask &currentTask() const;
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_FRONTEND_ARA_SEQUENCER_HH__
