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
#include <initializer_list>
#include <limits>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "cpu/vector_engine/common/vector_support.hh"
#include "cpu/vector_engine/frontend/ara_sequencer.hh"
#include "cpu/vector_engine/frontend/command_queue.hh"
#include "cpu/vector_engine/frontend/task_distributor.hh"
#include "cpu/vector_engine/vpu/interconnect/interconnect_stats.hh"
#include "cpu/vector_engine/vpu/interconnect/vector_interconnect.hh"
#include "cpu/vector_engine/vpu/lanes/ara_lane.hh"
#include "cpu/vector_engine/vpu/register_file/lane_register_file.hh"
#include "cpu/vector_engine/vpu/slide/slide_unit.hh"
#include "cpu/vector_engine/vpu/vlsu/ara_vlsu.hh"
#include "debug/VectorTimeline.hh"
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
    VpuParameters config;
    config.vrf = {params.vlen_bytes, params.lane_word_bytes, params.num_lanes,
                  params.banks_per_lane};
    config.commandQueueEntries = params.command_queue_entries;
    config.maxActiveCommands = params.max_active_commands;
    config.elenBits = params.elen_bits;
    const auto word_bytes = config.vrf.laneWordBytes;
    // Cero significa heredar la palabra física, no un recurso de cero bytes.
    // La resolución sucede una vez, antes de construir cualquier consumidor.
    const auto resolve_width = [word_bytes](uint32_t width) {
        if (width == 0) {
            return word_bytes;
        }
        return width;
    };
    // Agrupar los parámetros por su consumidor: lane y unidades de cálculo,
    // bancos del VRF, peticiones vectoriales y conexión con memoria.
    config.lane.taskEntries = params.lane_task_entries;
    config.lane.maxBundleBytes = resolve_width(params.max_bundle_bytes);
    config.lane.alusPerLane = params.alus_per_lane;
    // Cada clase de unidad tiene sus buffers de operandos y resultados.
    auto &alu = config.lane.alu;
    alu.unit = {resolve_width(params.alu_width_bytes), params.alu_capacity,
                params.alu_operand_entries_per_source,
                params.alu_result_entries};
    alu.add = {params.alu_add_latency, params.alu_add_initiation_interval};
    alu.multiply = {params.alu_multiply_latency,
                    params.alu_multiply_initiation_interval};
    alu.minimum = {params.alu_minimum_latency,
                   params.alu_minimum_initiation_interval};
    // Una latencia por SEW, en el orden 8/16/32/64 de divideLatency.
    fatal_if(params.alu_divide_latencies.size() != alu.divideLatency.size(),
             "alu_divide_latencies requires one value per SEW: 8/16/32/64");
    for (std::size_t i = 0; i < alu.divideLatency.size(); ++i) {
        alu.divideLatency[i] = Cycles(params.alu_divide_latencies[i]);
    }
    auto &fpu = config.lane.fpu;
    fpu.unit = {resolve_width(params.fpu_width_bytes), params.fpu_capacity,
                params.fpu_operand_entries_per_source,
                params.fpu_result_entries};
    fpu.addMultiply = {params.fpu_add_multiply_latency,
                       params.fpu_add_multiply_initiation_interval};
    fpu.divideLatency = params.fpu_divide_latency;
    config.bank = {
        params.bank_read_ports,       params.bank_write_ports,
        params.bank_read_write_ports, params.bank_access_entries,
        params.bank_latency,          params.bank_initiation_interval};
    config.vlsu = {params.memory_max_block_bytes, params.memory_active_blocks,
                   params.memory_vrf_inputs_per_lane};
    config.memory = {params.address_bits,
                     params.memory_max_outstanding_packets};
    // La red se construye siempre; el modelo se elige aquí, una sola vez.
    fatal_if(params.interconnect_model != "crossbar",
             "Only the crossbar interconnect model is implemented");
    config.interconnect.model = InterconnectModelKind::Crossbar;
    config.interconnect.capabilities = {
        config.vrf.numLanes,
        resolve_width(params.interconnect_max_packet_bytes),
        params.interconnect_packets_per_destination,
        resolve_width(params.interconnect_bytes_per_destination),
        resolve_width(params.interconnect_source_bytes_per_cycle),
        resolve_width(params.interconnect_destination_bytes_per_cycle)};
    config.interconnect.transitLatency = params.interconnect_transit_latency;
    const auto &network = config.interconnect.capabilities;
    fatal_if(!network.maxPacketBytes || !network.packetsPerDestination ||
                 network.bytesPerDestination < network.maxPacketBytes ||
                 !network.sourceBytesPerCycle ||
                 !network.destinationBytesPerCycle,
             "Invalid interconnect capacity contract");
    config.slide = {params.slide_vrf_inputs_per_lane,
                    params.slide_source_buffers_per_lane,
                    params.slide_assembly_buffers_per_lane};
    fatal_if(!config.slide.currentProfile(),
             "Slide unit requires positive VRF inputs and source buffers "
             "and at least two assembly buffers per lane");

    // Comprobar primero dimensiones y límites generales de esta fase.
    const auto bytes = config.vrf.vlenBytes;
    fatal_if(bytes < 4 || bytes > 8192 || (bytes & (bytes - 1)),
             "VectorEngine requires a power-of-two VLEN of 4..8192 bytes");
    // Cada comando activo conserva su crédito en la cola: no puede haber
    // más activos que entradas. La fase 5 admite varios activos.
    fatal_if(!config.commandQueueEntries || !config.maxActiveCommands ||
                 config.maxActiveCommands > config.commandQueueEntries,
             "VectorEngine requires 1 <= max_active_commands <= "
             "command_queue_entries");
    fatal_if(params.elen_bits != 32 && params.elen_bits != 64,
             "VectorEngine requires ELEN of 32 or 64 bits");
    fatal_if(uint64_t{bytes} * 8 < config.elenBits,
             "VectorEngine requires VLEN >= ELEN");
    fatal_if(!config.lane.currentProfile(word_bytes) ||
                 !config.bank.currentProfile(),
             "Invalid lane or bank resource profile: positive capacities, "
             "buffers and timings, unit widths equal to lane_word_bytes");
    // Check products before constructing any resource. A larger count may
    // be legal but underutilized when another finite stage is the bottleneck.
    const auto check_extent = [](std::initializer_list<uint64_t> factors) {
        uint64_t size = 1;
        for (const auto factor : factors) {
            fatal_if(factor && size > std::numeric_limits<std::size_t>::max() /
                                          factor,
                     "VPU resource byte extent exceeds host address space");
            size *= factor;
        }
    };
    check_extent({config.vrf.numLanes, config.lane.taskEntries, 3,
                  sizeof(VrfRequestPort)});
    check_extent({config.vrf.numLanes, config.vlsu.vrfInputsPerLane,
                  sizeof(VrfRequestPort)});
    check_extent({config.vrf.numLanes, config.slide.vrfInputsPerLane,
                  sizeof(VrfRequestPort)});
    check_extent({config.vrf.numLanes, config.slide.assemblyBuffersPerLane,
                  word_bytes});
    check_extent(
        {config.vrf.numLanes, config.slide.sourceBuffersPerLane, word_bytes});
    for (const auto *unit : {&alu.unit, &fpu.unit}) {
        check_extent({config.vrf.numLanes, unit->operandEntriesPerSource, 2,
                      word_bytes});
        check_extent({config.vrf.numLanes, unit->resultEntries, word_bytes});
    }
    check_extent({config.vrf.numLanes, config.lane.alusPerLane,
                  alu.unit.capacity, 3, word_bytes});
    // The FPU divider adds one bundle to the add/multiply pipeline capacity.
    check_extent(
        {config.vrf.numLanes, uint64_t{fpu.unit.capacity} + 1, 3, word_bytes});
    check_extent({config.vrf.numLanes, config.vrf.banksPerLane,
                  config.bank.accessEntries, word_bytes});
    fatal_if(config.memory.addressBits != 32 &&
                 config.memory.addressBits != 64,
             "VectorEngine requires a 32-bit or 64-bit address width");
    // Fase 4: un bloque unit-stride activo y un paquete físico en vuelo.
    fatal_if(config.memory.maxOutstandingPackets != 1 ||
                 config.vlsu.activeBlocks != 1 ||
                 !config.vlsu.vrfInputsPerLane,
             "Memory profile has one block/packet and positive VLSU VRF "
             "inputs");
    fatal_if(params.mapping_policy != "word_striped",
             "Only word_striped VRF mapping is implemented");
    fatal_if(params.target_byte_order != ByteOrder::little &&
                 params.target_byte_order != ByteOrder::big,
             "VectorEngine requires a valid target byte order");
    fatal_if(params.supported_sews.empty() || params.supported_lmuls.empty(),
             "VectorEngine requires nonempty SEW and LMUL profiles");
    fatal_if(!config.vlsu.maxBlockBytes,
             "Memory block byte limit must be positive");
    // Cada tamaño habilitado debe caber entero en una palabra y un bloque.
    for (const auto sew : params.supported_sews) {
        fatal_if(sew > config.elenBits ||
                     !VectorSupport::supportsElementWidth(sew),
                 "Configured SEW is not implemented in the current backend");
        fatal_if(std::find(config.supportedSews.begin(),
                           config.supportedSews.end(),
                           sew) != config.supportedSews.end(),
                 "VectorEngine has a duplicate supported SEW");
        const auto element_bytes = VectorSupport::elementBytes(sew);
        fatal_if(!word_bytes || word_bytes % element_bytes ||
                     config.vlsu.maxBlockBytes < element_bytes ||
                     config.vlsu.maxBlockBytes % element_bytes,
                 "Word and block limits must contain whole elements");
        config.supportedSews.push_back(sew);
    }
    // Un LMUL sólo es útil si admite al menos uno de los tamaños elegidos.
    for (const int exponent : params.supported_lmuls) {
        fatal_if(exponent < -3 || exponent > 3,
                 "VectorEngine LMUL exponent must be in [-3, 3]");
        const auto lmul = static_cast<VectorLmul>(exponent);
        fatal_if(std::find(config.supportedLmuls.begin(),
                           config.supportedLmuls.end(),
                           lmul) != config.supportedLmuls.end(),
                 "VectorEngine has a duplicate supported LMUL");
        const bool usable = std::any_of(
            config.supportedSews.begin(), config.supportedSews.end(),
            [&](uint16_t sew) {
                return VectorSupport::inTargetProfile(sew, lmul,
                                                      config.elenBits) &&
                       VectorSupport::maxElements(bytes, lmul, sew) != 0;
            });
        fatal_if(!usable, "Configured LMUL has no supported SEW combination");
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
      stats(this, "commands", "dispatched vector commands"),
      vpuParameters(makeConfiguration(params)),
      addressBits(params.address_bits),
      targetByteOrder(params.target_byte_order),
      mapper(vpuParameters.vrf, *this, vpuParameters.mapping),
      // Revisa si todos los módulos han terminado durante drain.
      drainEvent([this] { checkDrain(); }, name() + ".drain")
{
    // Los constructores sólo guardan callbacks. Ninguno puede emitir trabajo
    // hasta completar el cableado, conectar la CPU y recibir dispatch.
    CpuCompletionEndpoint &completion_endpoint = *this;

    // La red y sus métricas comunes existen antes que sus productores y
    // receptores. Las métricas sólo observan; el modelo no las conoce.
    interconnectStats =
        std::make_unique<InterconnectStats>(this, vpuParameters.vrf.numLanes);
    network = makeInterconnect(*this, *interconnectStats,
                               vpuParameters.interconnect);
    network->setObserver(interconnectStats->observer());

    commandQueue = std::make_unique<CommandQueue>(
        *this, vpuParameters.commandQueueEntries, vpuParameters.vrf.vlenBytes,
        vpuParameters.supportedLmuls, completion_endpoint,
        vpuParameters.elenBits);

    sequencer = std::make_unique<AraSequencer>(
        *this, *commandQueue, completion_endpoint, vpuParameters.vrf.vlenBytes,
        vpuParameters.maxActiveCommands,
        // Entrega la tarea aritmética al distribuidor de lanes.
        [this](const ArithmeticTask &task) {
            return distributor->acceptTask(task);
        },
        // Entrega la tarea de carga o store a la VLSU.
        [this](const MemoryTask &task) { return vlsu->acceptTask(task); },
        // Entrega la slide a SlideUnit.
        [this](const SlideTask &task) { return slideUnit->acceptTask(task); });

    // Cada comando aritmético activo tiene como máximo una tarea en el
    // distribuidor, de modo que su capacidad deriva de max_active_commands.
    distributor = std::make_unique<TaskDistributor>(
        *this, mapper, vpuParameters.maxActiveCommands,
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
            *this, mapper, vpuParameters.bank, lane_id,
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
                [this](const WriteAck &ack) { vlsu->recvVrfWriteAck(ack); }},
            LaneRegisterFile::ResponsePort{
                // SlideUnit recibe las palabras fuente leídas.
                [this](const ReadResponse &response) {
                    slideUnit->recvVrfReadResponse(response);
                },
                // Y la confirmación de cada palabra destino escrita.
                [this](const WriteAck &ack) {
                    slideUnit->recvVrfWriteAck(ack);
                }}));

        lanes.push_back(std::make_unique<AraLane>(
            *this, mapper, vpuParameters.lane, lane_id, targetByteOrder,
            // Producer storage is stable until the LRF grants the offer.
            [this, lane_id] { registerFiles[lane_id]->wakeup(); },
            // Devuelve al distribuidor el fragmento terminado.
            [this](const LaneCompletion &completion) {
                distributor->recvCompletion(completion);
            }));
    }

    vlsu = std::make_unique<AraVLSU>(
        *this, mapper, vpuParameters.vlsu, addressBits,
        // Entrega al backend la petición lógica de memoria.
        [this](const VectorMemoryRequest &request) {
            return memoryBackend->acceptRequest(request);
        },
        // Fronteras de línea y página del contexto, para formar bloques.
        [this](ContextID context_id) {
            return memoryBackend->boundaries(context_id);
        },
        // Both load writeback and store reads arbitrate in the owning LRF.
        [this](LaneId lane_id) {
            panic_if(lane_id >= registerFiles.size(),
                     "VectorEngine received an invalid VRF destination");
            registerFiles[lane_id]->wakeup();
        },
        // Informa al sequencer de la carga o store terminados.
        [this](const UnitCompletion &completion) {
            sequencer->recvCompletion(completion);
        });

    memoryBackend = std::make_unique<VectorMemoryBackend>(
        *this, vpuParameters.memory,
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

    // Bind every persistent input before any module can receive work.
    slideUnit = std::make_unique<SlideUnit>(
        *this, mapper, vpuParameters.slide, *network, targetByteOrder,
        // Lecturas de fuente y escrituras de destino arbitran en su LRF.
        [this](LaneId lane_id) {
            panic_if(lane_id >= registerFiles.size(),
                     "VectorEngine received an invalid VRF destination");
            registerFiles[lane_id]->wakeup();
        },
        // Informa al sequencer de la slide terminada.
        [this](const UnitCompletion &completion) {
            sequencer->recvCompletion(completion);
        });

    // Context/role indices precede the VLSU inputs, then the slide inputs;
    // the order is stable. Each lane receives its network packets through
    // its slide agent.
    for (LaneId lane_id = 0; lane_id < num_lanes; ++lane_id) {
        for (auto &port : lanes[lane_id]->vrfPorts()) {
            registerFiles[lane_id]->bindRequestPort(std::move(port));
        }
        for (uint32_t input = 0; input < vpuParameters.vlsu.vrfInputsPerLane;
             ++input) {
            registerFiles[lane_id]->bindRequestPort(
                vlsu->vrfPort(lane_id, input));
        }
        for (uint32_t input = 0; input < vpuParameters.slide.vrfInputsPerLane;
             ++input) {
            registerFiles[lane_id]->bindRequestPort(
                slideUnit->vrfPort(lane_id, input));
        }
        network->bindReceiver(lane_id, slideUnit->receiver(lane_id));
    }
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
    // LRF idle checks consult bound producer ports. Destroy the arbiters
    // while lanes, VLSU and slide unit still exist, before reverse member
    // destruction.
    registerFiles.clear();
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
    fatal_if(binding.vlenBytes != vpuParameters.vrf.vlenBytes ||
                 binding.elenBits != vpuParameters.elenBits,
             "VectorEngine VLEN/ELEN differ from the CPU");
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
        ++stats.rejected;
        return GrantResult::rejected(RejectionReason::InvalidIdentity);
    }
    // Revisar soporte y habilitación antes de preguntar por espacio en cola.
    if (const auto reason = VectorSupport::checkCommand(
            command, vpuParameters.vrf.vlenBytes, vpuParameters.supportedLmuls,
            vpuParameters.elenBits)) {
        ++stats.rejected;
        return GrantResult::rejected(*reason);
    }
    if (std::find(vpuParameters.supportedSews.begin(),
                  vpuParameters.supportedSews.end(), command.config.sewBits) ==
        vpuParameters.supportedSews.end()) {
        ++stats.rejected;
        return GrantResult::rejected(
            RejectionReason::UnsupportedConfiguration);
    }
    const auto result = commandQueue->requestGrant(command);
    if (result.status == GrantStatus::Stall) {
        ++stats.retries;
    } else if (result.status == GrantStatus::Rejected) {
        ++stats.rejected;
    }
    return result;
}

/**
 * Consume la reserva en la cola y despierta al sequencer.
 * CpuVectorInterface lo llama al entregar un comando concedido.
 */
void
VectorEngine::dispatch(const GrantToken &token, const VectorCommand &command)
{
    requireCpuBinding();
    uint64_t count = 0;
    if (command.config.vl > command.config.vstart) {
        count = command.config.vl - command.config.vstart;
    }
    stats.accept(count, uint64_t{count} * VectorSupport::elementBytes(
                                              command.config.sewBits));
    DPRINTF(VectorTimeline, "dispatch cmd=%llu pc=%#x vl=%u\n",
            command.command.commandId, command.pc, command.config.vl);
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
    stats.complete(completion.status != CompletionStatus::Success);
    DPRINTF(VectorTimeline, "complete cmd=%llu status=%d\n",
            completion.command.commandId, static_cast<int>(completion.status));
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
        !distributor->isIdle() || !vlsu->isIdle() || !slideUnit->isIdle() ||
        !network->isIdle() || !memoryBackend->isIdle()) {
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
    network->beginDrain();
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
