// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__

#include <deque>
#include <functional>

#include "cpu/vector_engine/common/execution_types.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/common/vpu_parameters.hh"
#include "cpu/vector_engine/vpu/register_file/vrf_port.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * ALU de suma entera de 8/16/32/64 bits, con pipeline de capacidad finita.
 * Latencia e intervalo de iniciación son ciclos independientes del reloj VPU.
 * Copia los bundles aceptados y reserva sus resultados. La lane reserva
 * también recepción antes de enviar: el callback de resultado no se bloquea.
 * No accede al VRF ni completa instrucciones. Propietario y receptor
 * sobreviven al módulo hasta completar drain; no se cancela trabajo activo.
 */
class Alu
{
  public:
    using ResultSender = std::function<void(const ExecutionResult &)>;

    Alu(ClockedObject &owner, LaneId lane_id, ResultSender send_result,
        statistics::Group &stats_parent,
        const ExecutionParameters &parameters);
    ~Alu();

    Alu(const Alu &) = delete;
    Alu &operator=(const Alu &) = delete;

    TransferResult acceptBundle(const ExecutionBundle &bundle);

    bool
    isIdle() const
    {
        return pending.empty() && !responding && !executeEvent.scheduled();
    }

  private:
    struct ExecutionState
    {
        ExecutionBundle bundle;
        ExecutionResult result;
        Tick readyAt;
    };

    ModuleStats stats;
    statistics::Scalar elements8;
    statistics::Scalar elements16;
    statistics::Scalar elements32;
    statistics::Scalar elements64;
    statistics::Scalar capacityStalls;
    statistics::Scalar intervalStalls;
    ClockedObject &owner;
    const ExecutionParameters &parameters;
    ResultSender sendResult;
    std::deque<ExecutionState> pending;
    Tick nextIssue = 0;
    bool responding = false;
    EventFunctionWrapper executeEvent;

    void execute();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__
