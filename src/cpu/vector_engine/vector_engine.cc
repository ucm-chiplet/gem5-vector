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

#include "cpu/vector_engine/vector_engine.hh"

#include <algorithm>
#include <utility>

#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "cpu/vector_engine/frontend/ara_sequencer.hh"
#include "cpu/vector_engine/frontend/command_queue.hh"
#include "cpu/vector_engine/frontend/task_distributor.hh"
#include "cpu/vector_engine/vpu/lanes/ara_lane.hh"
#include "cpu/vector_engine/vpu/register_file/lane_register_file.hh"
#include "cpu/vector_engine/vpu/vlsu/ara_vlsu.hh"
#include "sim/system.hh"

namespace gem5::vector_engine
{

/**
 * Convierte los parámetros del SimObject en una configuración única.
 * El constructor la usa antes de crear el mapper y los módulos.
 */
VpuParameters
VectorEngine::makeConfiguration(const Params &params)
{
    VpuParameters config{
        {params.vlen_bytes, params.lane_word_bytes, params.num_lanes,
         params.banks_per_lane},
        {},
        params.command_queue_entries,
        params.lane_task_entries,
        params.operand_buffer_entries_per_source,
        params.result_buffer_entries
    };

    // VLEN RVV es una potencia de dos, hasta 65536 bits. Este baseline
    // requiere al menos un elemento de 32 bits en cada palabra de lane.
    const auto bytes = config.vrf.vlenBytes;
    fatal_if(bytes < 4 || bytes > 8192 || (bytes & (bytes - 1)),
             "VectorEngine requires a power-of-two VLEN of 4..8192 bytes");
    fatal_if(!config.commandQueueEntries,
             "VectorEngine requires at least one command entry");
    fatal_if(config.laneTaskEntries != 1 ||
                 config.operandBufferEntriesPerSource != 1 ||
                 config.resultBufferEntries != 1,
             "VectorEngine baseline requires one lane task, one buffer "
             "per operand source and one result buffer");
    fatal_if(params.address_bits != 32 && params.address_bits != 64,
             "VectorEngine requires a 32-bit or 64-bit address width");
    fatal_if(params.target_byte_order != ByteOrder::little &&
                 params.target_byte_order != ByteOrder::big,
             "VectorEngine requires a valid target byte order");
    fatal_if(params.supported_lmuls.empty(),
             "VectorEngine requires explicit supported LMULs");

    for (const int exponent : params.supported_lmuls) {
        // Validar antes del cast al enum de 8 bits, sin truncar el parámetro.
        fatal_if(exponent < -3 || exponent > 3,
                 "VectorEngine LMUL exponent must be in [-3, 3]");
        const auto lmul = static_cast<VectorLmul>(exponent);
        fatal_if(std::find(config.supportedLmuls.begin(),
                           config.supportedLmuls.end(), lmul) !=
                     config.supportedLmuls.end(),
                 "VectorEngine has a duplicate supported LMUL");
        const uint64_t effective_bytes = exponent >= 0
            ? uint64_t{bytes} << exponent : uint64_t{bytes} >> -exponent;
        fatal_if(effective_bytes < 4,
                 "VectorEngine supported LMUL cannot hold a 32-bit element");
        config.supportedLmuls.push_back(lmul);
    }

    // AddressMapper comprueba las dimensiones y su alineación al construirlo.
    return config;
}

/**
 * Construye los módulos y conecta sus envíos y respuestas.
 * gem5 lo invoca al crear el SimObject desde VectorEngine.py.
 */
VectorEngine::VectorEngine(const Params &params)
    : ClockedObject(params),
      vpuParameters(makeConfiguration(params)),
      addressBits(params.address_bits),
      targetByteOrder(params.target_byte_order),
      mapper(vpuParameters.vrf),
      // Revisa si todos los módulos han terminado durante drain.
      drainEvent([this] { checkDrain(); }, name() + ".drain")
{
    // Los constructores sólo guardan callbacks. Ninguno puede emitir trabajo
    // hasta completar el cableado, conectar la CPU y recibir dispatch.
    CpuCompletionEndpoint &completion_endpoint = *this;
    commandQueue = std::make_unique<CommandQueue>(
        vpuParameters.commandQueueEntries, vpuParameters.vrf.vlenBytes,
        vpuParameters.supportedLmuls, completion_endpoint);

    sequencer = std::make_unique<AraSequencer>(
        *this, *commandQueue, completion_endpoint, vpuParameters.vrf.vlenBytes,
        // Entrega la tarea aritmética al distribuidor de lanes.
        [this](const ArithmeticTask &task) {
            return distributor->acceptTask(task);
        },
        // Entrega la tarea de carga o store a la VLSU.
        [this](const MemoryTask &task) { return vlsu->acceptTask(task); });

    distributor = std::make_unique<TaskDistributor>(
        *this, mapper,
        // Envía cada fragmento a la lane indicada por la tarea.
        [this](const LaneTask &task) {
            panic_if(task.laneId >= lanes.size(),
                     "VectorEngine received an invalid lane destination");
            return lanes[task.laneId]->acceptTask(task);
        },
        // Devuelve al sequencer la tarea aritmética ya agregada.
        [this](const UnitCompletion &completion) {
            sequencer->recvCompletion(completion);
        });

    const auto num_lanes = vpuParameters.vrf.numLanes;
    registerFiles.reserve(num_lanes);
    lanes.reserve(num_lanes);
    for (LaneId lane_id = 0; lane_id < num_lanes; ++lane_id) {
        registerFiles.push_back(std::make_unique<LaneRegisterFile>(
            *this, mapper, lane_id,
            LaneRegisterFile::ResponsePort{
                // La lectura de operandos vuelve a la lane solicitante.
                [this, lane_id](const ReadResponse &response) {
                    lanes[lane_id]->recvVrfReadResponse(response);
                },
                // La lane puede completar tras confirmar su writeback.
                [this, lane_id](const WriteAck &ack) {
                    lanes[lane_id]->recvVrfWriteAck(ack);
                }},
            LaneRegisterFile::ResponsePort{
                // En un store, la VLSU recibe los datos leídos del VRF.
                [this](const ReadResponse &response) {
                    vlsu->recvVrfReadResponse(response);
                },
                // En una carga, el VRF confirma la escritura a la VLSU.
                [this](const WriteAck &ack) {
                    vlsu->recvVrfWriteAck(ack);
                }}));

        lanes.push_back(std::make_unique<AraLane>(
            *this, mapper, lane_id, targetByteOrder,
            // Lee los operandos de la slice local del VRF.
            [this, lane_id](const VrfReadRequest &request) {
                return registerFiles[lane_id]->read(request);
            },
            // Escribe el resultado en la slice local del VRF.
            [this, lane_id](const VrfWriteRequest &request) {
                return registerFiles[lane_id]->write(request);
            },
            // Devuelve al distribuidor el fragmento terminado.
            [this](const LaneCompletion &completion) {
                distributor->recvCompletion(completion);
            }));
    }

    vlsu = std::make_unique<AraVLSU>(
        *this, mapper, addressBits,
        // Entrega al backend la petición lógica de memoria.
        [this](const VectorMemoryRequest &request) {
            return memoryBackend->acceptRequest(request);
        },
        // Para un store, lee los datos de la lane propietaria.
        [this](LaneId lane_id, const VrfReadRequest &request) {
            panic_if(lane_id >= registerFiles.size(),
                     "VectorEngine received an invalid VRF destination");
            return registerFiles[lane_id]->read(request);
        },
        // Para una carga, escribe los datos en la lane propietaria.
        [this](LaneId lane_id, const VrfWriteRequest &request) {
            panic_if(lane_id >= registerFiles.size(),
                     "VectorEngine received an invalid VRF destination");
            return registerFiles[lane_id]->write(request);
        },
        // Informa al sequencer de la carga o store terminados.
        [this](const UnitCompletion &completion) {
            sequencer->recvCompletion(completion);
        });

    memoryBackend = std::make_unique<VectorMemoryBackend>(
        *this, addressBits,
        // Recupera el ThreadContext para traducir la petición.
        [this](ContextID context_id) { return resolveContext(context_id); },
        // Construye un fault de acceso con la semántica de la CPU.
        [this](Addr address, MemoryDirection direction) {
            requireCpuBinding();
            const Fault fault = cpuBinding.accessFault(address, direction);
            panic_if(fault == NoFault,
                     "VectorEngine access fault factory returned NoFault");
            return fault;
        },
        // Devuelve a la VLSU la respuesta lógica de memoria.
        [this](const VectorMemoryResponse &response) {
            vlsu->recvMemoryResponse(response);
        });
}

/**
 * Comprueba que no quede trabajo y cancela la observación de drain.
 * Se ejecuta al destruir el SimObject y sus módulos.
 */
VectorEngine::~VectorEngine()
{
    panic_if(!isIdle(), "Destroying VectorEngine before its work drained");
    if (drainEvent.scheduled()) {
        deschedule(drainEvent);
    }
}

/**
 * Conecta el receptor y los servicios arquitectónicos de MinorCPU.
 * La integración CPU--VPU debe llamarlo antes de init y del offload.
 */
void
VectorEngine::bindCpu(CpuCompletionEndpoint &endpoint, CpuBinding binding)
{
    fatal_if(cpuEndpoint, "VectorEngine CPU endpoint is already bound");
    fatal_if(binding.contextId == InvalidContextID ||
                 !binding.resolveContext || !binding.accessFault,
             "VectorEngine requires a context and CPU memory services");
    fatal_if(binding.vlenBytes != vpuParameters.vrf.vlenBytes,
             "VectorEngine VLEN differs from its CPU VLEN");
    fatal_if(binding.addressBits != addressBits ||
                 binding.byteOrder != targetByteOrder,
             "VectorEngine address width or byte order differs from its CPU");
    cpuBinding = std::move(binding);
    cpuEndpoint = &endpoint;
}

/**
 * Impide usar servicios o callbacks de CPU antes de conectarla.
 * Lo consultan init, admisión y las rutas de memoria y finalización.
 */
void
VectorEngine::requireCpuBinding() const
{
    fatal_if(!cpuEndpoint, "VectorEngine requires bindCpu before use");
}

/**
 * Obtiene y comprueba el contexto asociado a un CommandKey.
 * Lo usa init y el backend al traducir accesos vectoriales.
 */
ThreadContext *
VectorEngine::resolveContext(ContextID context_id) const
{
    requireCpuBinding();
    panic_if(context_id != cpuBinding.contextId,
             "VectorEngine received a different CPU context");
    ThreadContext *context = cpuBinding.resolveContext(context_id);
    fatal_if(!context || context->contextId() != context_id,
             "VectorEngine resolver did not return the bound CPU context");
    return context;
}

/**
 * Verifica la conexión de memoria y el modo timing tras el cableado.
 * gem5 lo llama durante la inicialización de los SimObjects.
 */
void
VectorEngine::init()
{
    ClockedObject::init();
    requireCpuBinding();
    fatal_if(!memoryBackend->getPort().isConnected(),
             "VectorEngine mem_side port is not connected");
    const auto *system = resolveContext(cpuBinding.contextId)->getSystemPtr();
    fatal_if(!system || !system->isTimingMode(),
             "VectorEngine requires timing memory mode");
}

/**
 * Expone el puerto timing del backend como mem_side.
 * gem5 lo consulta al conectar los puertos de la configuración.
 */
Port &
VectorEngine::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side") {
        fatal_if(idx != InvalidPortID,
                 "VectorEngine mem_side is a scalar port");
        return memoryBackend->getPort();
    }
    return ClockedObject::getPort(if_name, idx);
}

/**
 * Comprueba el contexto y delega la reserva de un comando.
 * CpuVectorInterface lo usa antes de despachar un offload.
 */
GrantResult
VectorEngine::requestGrant(const VectorCommand &command)
{
    requireCpuBinding();
    if (!command.command.valid() ||
        command.command.contextId != cpuBinding.contextId) {
        return GrantResult::rejected(RejectionReason::InvalidIdentity);
    }
    // La validación estructural completa pertenece a CpuVectorInterface.
    return commandQueue->requestGrant(command);
}

/**
 * Consume la reserva en la cola y despierta al sequencer.
 * CpuVectorInterface lo llama al entregar un comando concedido.
 */
void
VectorEngine::dispatch(const GrantToken &token, const VectorCommand &command)
{
    requireCpuBinding();
    commandQueue->dispatch(token, command);
    sequencer->wakeup();
}

/**
 * Reenvía a MinorCPU la aceptación, distinta de la finalización.
 * CommandQueue lo llama después de insertar el comando en la FIFO.
 */
void
VectorEngine::accepted(CommandKey command)
{
    requireCpuBinding();
    cpuEndpoint->accepted(command);
}

/**
 * Reenvía a MinorCPU el resultado terminal del comando.
 * AraSequencer lo llama tras cerrar la tarea o un rango vacío.
 */
void
VectorEngine::completed(const VectorCompletion &completion)
{
    requireCpuBinding();
    cpuEndpoint->completed(completion);
}

/**
 * Comprueba que cola, unidades, VRF y memoria no tengan trabajo.
 * Lo usan drain, drainResume y el destructor para cerrar la VPU.
 */
bool
VectorEngine::isIdle() const
{
    if (!commandQueue->isIdle() || !sequencer->isIdle() ||
        !distributor->isIdle() || !vlsu->isIdle() ||
        !memoryBackend->isIdle()) {
        return false;
    }
    for (const auto &lane : lanes) {
        if (!lane->isIdle()) {
            return false;
        }
    }
    for (const auto &lrf : registerFiles) {
        if (!lrf->isIdle()) {
            return false;
        }
    }
    return true;
}

/**
 * Detiene nuevas reservas y espera el trabajo ya admitido.
 * gem5 lo llama al vaciar los SimObjects; Minor vacía su estado aparte.
 */
DrainState
VectorEngine::drain()
{
    commandQueue->beginDrain();
    if (isIdle()) {
        if (drainEvent.scheduled()) {
            deschedule(drainEvent);
        }
        return DrainState::Drained;
    }
    scheduleDrainCheck();
    return DrainState::Draining;
}

/**
 * Programa una sola comprobación de vaciado en el siguiente ciclo.
 * Lo usan drain y checkDrain mientras quede trabajo pendiente.
 */
void
VectorEngine::scheduleDrainCheck()
{
    // Sólo observa durante drain; no reintenta trabajo ni paquetes de memoria.
    if (!drainEvent.scheduled()) {
        schedule(drainEvent, clockEdge(Cycles(1)));
    }
}

/**
 * Avisa al gestor de drain al quedar inactiva toda la VPU.
 * Lo ejecuta drainEvent y se reprograma mientras haya trabajo.
 */
void
VectorEngine::checkDrain()
{
    if (isIdle()) {
        signalDrainDone();
    } else {
        scheduleDrainCheck();
    }
}

/**
 * Reabre la admisión después de completar el vaciado local.
 * gem5 lo llama al reanudar la simulación tras drain.
 */
void
VectorEngine::drainResume()
{
    panic_if(!isIdle() || drainEvent.scheduled(),
             "Cannot resume VectorEngine before drain finishes");
    commandQueue->endDrain();
    ClockedObject::drainResume();
}

/**
 * Rechaza checkpoints porque aún no existe formato para el VRF.
 * gem5 lo llamaría al guardar el estado del SimObject.
 */
void
VectorEngine::serialize(CheckpointOut &cp) const
{
    fatal("VectorEngine checkpoints are not implemented");
}

/**
 * Rechaza restaurar un checkpoint sin estado vectorial completo.
 * gem5 lo llamaría al cargar el estado del SimObject.
 */
void
VectorEngine::unserialize(CheckpointIn &cp)
{
    fatal("VectorEngine checkpoint restore is not implemented");
}

} // namespace gem5::vector_engine
