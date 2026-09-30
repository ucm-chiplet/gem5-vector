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

#ifndef __CPU_VECTOR_ENGINE_VECTOR_ENGINE_HH__
#define __CPU_VECTOR_ENGINE_VECTOR_ENGINE_HH__

#include <memory>
#include <vector>

#include "cpu/vector_engine/common/vpu_parameters.hh"
#include "cpu/vector_engine/interface/cpu_vector_interface.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "cpu/vector_engine/vpu/vlsu/vector_memory_backend.hh"
#include "enums/ByteOrder.hh"
#include "params/VectorEngine.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

class AraLane;
class AraSequencer;
class AraVLSU;
class CommandQueue;
class LaneRegisterFile;
class TaskDistributor;

/**
 * Propietario y punto de conexión de la VPU Ara-like en modo SE timing.
 * La frontera CPU valida los descriptores y asigna CommandKey antes de
 * llamar a este endpoint. No se reciben instrucciones ni se decodifican.
 *
 * Minor debe conectar bindCpu antes de init, ordenar el offload respecto a
 * su LSQ y consumir incluso durante drain los tokens ya concedidos. El
 * receptor y los servicios de CPU sobreviven a la VPU y no pueden destruirla
 * desde un callback. El propietario sólo se destruye después de drain.
 * El vaciado local no certifica el vaciado del pipeline ni del estado CPU.
 */
class VectorEngine : public ClockedObject,
                     public VpuCommandEndpoint,
                     private CpuCompletionEndpoint
{
  public:
    using Params = VectorEngineParams;

    /** Metadatos del contexto conectado, suministrados por la frontera CPU. */
    struct CpuBinding
    {
        ContextID contextId = InvalidContextID;
        uint32_t vlenBytes = 0;
        unsigned addressBits = 0;
        ByteOrder byteOrder = ByteOrder::Num_ByteOrder;
        VectorMemoryBackend::ContextResolver resolveContext;
        VectorMemoryBackend::AccessFaultFactory accessFault;
    };

    explicit VectorEngine(const Params &params);
    ~VectorEngine() override;

    void bindCpu(CpuCompletionEndpoint &endpoint, CpuBinding binding);
    void init() override;
    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    // Precondición: descriptor validado por CpuVectorInterface.
    GrantResult requestGrant(const VectorCommand &command) override;
    void dispatch(const GrantToken &token,
                  const VectorCommand &command) override;

    const VpuParameters &configuration() const { return vpuParameters; }
    bool isIdle() const;
    DrainState drain() override;
    void drainResume() override;

    // El VRF todavía no tiene formato de checkpoint: no perderlo en silencio.
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

  private:
    // Declarados antes que los consumidores para sobrevivir a sus vistas.
    const VpuParameters vpuParameters;
    const unsigned addressBits;
    const ByteOrder targetByteOrder;
    const AddressMapper mapper;

    CpuCompletionEndpoint *cpuEndpoint = nullptr;
    CpuBinding cpuBinding;

    std::unique_ptr<CommandQueue> commandQueue;
    std::unique_ptr<AraSequencer> sequencer;
    std::unique_ptr<TaskDistributor> distributor;
    std::vector<std::unique_ptr<LaneRegisterFile>> registerFiles;
    std::vector<std::unique_ptr<AraLane>> lanes;
    std::unique_ptr<AraVLSU> vlsu;
    std::unique_ptr<VectorMemoryBackend> memoryBackend;
    EventFunctionWrapper drainEvent;

    static VpuParameters makeConfiguration(const Params &params);
    void requireCpuBinding() const;
    ThreadContext *resolveContext(ContextID context_id) const;
    void accepted(CommandKey command) override;
    void completed(const VectorCompletion &completion) override;
    void scheduleDrainCheck();
    void checkDrain();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VECTOR_ENGINE_HH__
