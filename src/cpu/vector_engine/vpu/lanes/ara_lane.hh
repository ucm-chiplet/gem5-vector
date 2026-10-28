// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__

#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "cpu/vector_engine/common/lane_completion.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/vpu/lanes/alu.hh"
#include "cpu/vector_engine/vpu/lanes/fpu.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "enums/ByteOrder.hh"

namespace gem5::vector_engine
{

/**
 * Lane con contextos acotados para fragmentos de una misma tarea aritmética.
 * Puede solapar lectura, cálculo y writeback sin admitir otra instrucción.
 * Reserva conjuntamente ambos buffers de operandos antes de publicar las
 * lecturas y reserva salida antes de enviar a una FU. El escalar capturado
 * ocupa su buffer replicado, pero no solicita un puerto de lectura al VRF.
 *
 * Los accesos se presentan persistentemente al LRF por contexto y papel.
 * Cada retorno se localiza por su clave, nunca por orden de llegada.
 * Sólo WriteAck permite completar el fragmento y liberar su resultado.
 * Las fuentes de cada fragmento se leen antes de escribir su destino; los
 * rangos de otros fragmentos de la tarea son disjuntos, incluso con alias
 * legal de igual ancho. No se introduce chaining ni predicación.
 *
 * Propietario, mapper, LRF y receptor sobreviven al módulo hasta isIdle().
 * Las respuestas se consumen sin rechazo porque su recepción está reservada.
 */
class AraLane
{
  public:
    using VrfWakeup = std::function<void()>;
    using CompletionSender = std::function<void(const LaneCompletion &)>;

    AraLane(ClockedObject &owner, const AddressMapper &mapper,
            const LaneParameters &parameters, LaneId lane_id,
            ByteOrder target_byte_order, VrfWakeup wake_vrf,
            CompletionSender send_completion);
    ~AraLane();
    AraLane(const AraLane &) = delete;
    AraLane &operator=(const AraLane &) = delete;

    // Un puerto estable por contexto y papel; se enlazan durante construcción.
    std::vector<VrfRequestPort> vrfPorts();
    TransferResult acceptTask(const LaneTask &task);
    void recvVrfReadResponse(const ReadResponse &response);
    void recvVrfWriteAck(const WriteAck &ack);
    void wakeup();
    bool isIdle() const;

  private:
    enum class Phase
    {
        Prepare,
        Read,
        Execute,
        WaitExecution,
        Write,
        Finish
    };
    enum class AccessRole : std::size_t
    {
        Lhs = 0,
        Rhs = 1,
        Destination = 2
    };
    struct PendingVrfAccess
    {
        VrfRequest request;
        Tick eligibleAt;
        Tick publishedAt;
        bool granted = false;
    };
    struct LaneFragmentState
    {
        LaneTask task;
        Phase phase = Phase::Prepare;
        ExecutionBundle bundle;
        bool lhsReady = false;
        bool rhsReady = false;
        bool operandsReserved = false;
        bool resultReserved = false;
        std::optional<ExecutionResult> result;
        std::array<std::optional<PendingVrfAccess>, 3> pending;
    };

    ModuleStats stats;
    ModuleStats lhsBuffers;
    ModuleStats rhsBuffers;
    ModuleStats resultBuffers;
    // Phase accounting within the result reservation, not another resource.
    ModuleStats writebacks;
    statistics::Scalar vrfReads;
    statistics::Scalar vrfWrites;
    statistics::Scalar operandWaitCycles;
    statistics::Scalar resultWaitCycles;
    statistics::Scalar executionRetries;
    ClockedObject &owner;
    const AddressMapper &mapper;
    const LaneParameters &parameters;
    const LaneId laneId;
    const ByteOrder targetByteOrder;
    VrfWakeup wakeVrf;
    CompletionSender sendCompletion;
    std::vector<std::optional<LaneFragmentState>> contexts;
    uint32_t operandBundles = 0;
    uint32_t resultBundles = 0;
    std::optional<TaskKey> accessTask;
    VrfAccessId nextAccessId = 0;
    LaneFragmentId lastFragmentId = InvalidLaneFragmentId;
    Alu alu;
    Fpu fpu;
    EventFunctionWrapper progressEvent;

    void validateTask(const LaneTask &task) const;
    VrfAccess makeAccess(const TaskKey &task, const VectorRegRef &reg,
                         const ByteRange &range);
    std::vector<std::size_t> orderedContexts() const;
    void prepareOperands(LaneFragmentState &state);
    void tryExecute(LaneFragmentState &state);
    std::optional<VrfOffer> peekAccess(std::size_t context,
                                       AccessRole role) const;
    void recvVrfGrant(std::size_t context, AccessRole role,
                      const VrfAccessKey &key);
    void evaluate();
    void recvExecutionResult(const ExecutionResult &result);
    void finish(std::size_t context);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__
