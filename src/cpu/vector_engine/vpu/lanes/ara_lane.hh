// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__

#include <array>
#include <functional>
#include <optional>
#include <variant>

#include "cpu/vector_engine/common/lane_completion.hh"
#include "cpu/vector_engine/vpu/lanes/alu.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "enums/ByteOrder.hh"

namespace gem5::vector_engine
{

/**
 * Una lane física: un fragmento activo, dos slots de operandos y un slot
 * de writeback. Sólo completa después de consumir el WriteAck del LRF.
 *
 * El propietario conecta los callbacks al LRF local y al distribuidor.
 * Los envíos aceptados se copian; los callbacks pueden ser síncronos, pero
 * no pueden destruir la lane. Propietario, mapper y receptores sobreviven
 * al módulo. El propietario coordina drain y espera isIdle() antes de
 * destruirlo; no hay cancelación ni reset con trabajo activo.
 *
 * target_byte_order describe el objetivo simulado. La configuración RVV y
 * su capacidad efectiva se validan antes de crear LaneTask; aquí se validan
 * sus rangos físicos y su pertenencia a esta lane mediante AddressMapper.
 */
class AraLane
{
  public:
    using ReadSender = std::function<TransferResult(const VrfReadRequest &)>;
    using WriteSender = std::function<TransferResult(const VrfWriteRequest &)>;
    using CompletionSender = std::function<void(const LaneCompletion &)>;

    AraLane(ClockedObject &owner, const AddressMapper &mapper, LaneId lane_id,
            ByteOrder target_byte_order, ReadSender send_read,
            WriteSender send_write, CompletionSender send_completion);
    ~AraLane();

    AraLane(const AraLane &) = delete;
    AraLane &operator=(const AraLane &) = delete;

    TransferResult acceptTask(const LaneTask &task);
    void recvVrfReadResponse(const ReadResponse &response);
    void recvVrfWriteAck(const WriteAck &ack);
    void wakeup();

    bool
    isIdle() const
    {
        return !active && alu.isIdle() && !progressEvent.scheduled();
    }

  private:
    enum class Phase
    {
        Read,
        Execute,
        WaitExecution,
        Write,
        Finish
    };
    enum class AccessPhase
    {
        Prepared,
        Waiting
    };
    enum class AccessRole
    {
        Lhs = 0,
        Rhs = 1,
        Destination = 2
    };

    struct PendingVrfAccess
    {
        std::variant<VrfReadRequest, VrfWriteRequest> request;
        LaneFragmentKey fragment;
        AccessRole role;
        AccessPhase phase = AccessPhase::Prepared;
    };

    struct LaneFragmentState
    {
        LaneTask task;
        Phase phase = Phase::Read;
        ExecutionBundle bundle;
        bool lhsReady = false;
        bool rhsReady = false;
        std::optional<ExecutionResult> result;
        // Índices de AccessRole; cada entrada se retira al consumir respuesta.
        std::array<std::optional<PendingVrfAccess>, 3> pending;
    };

    ClockedObject &owner;
    const AddressMapper &mapper;
    const LaneId laneId;
    const ByteOrder targetByteOrder;
    ReadSender sendRead;
    WriteSender sendWrite;
    CompletionSender sendCompletion;
    std::optional<LaneFragmentState> active;
    // Se conservan entre fragmentos: accessId no se reinicia por fragmento.
    std::optional<TaskKey> accessTask;
    VrfAccessId nextAccessId = 0;
    LaneFragmentId lastFragmentId = InvalidLaneFragmentId;
    Alu alu;
    EventFunctionWrapper progressEvent;

    void validateTask(const LaneTask &task) const;
    VrfAccess makeAccess(const VectorRegRef &reg, const ByteRange &range);
    bool hasReadyWork() const;
    void evaluate();
    void sendAccess(AccessRole role);
    void recvExecutionResult(const ExecutionResult &result);
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__
