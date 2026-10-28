// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__

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
 * FPU interna binary16: FloatAdd con redondeo y flags por bundle.
 * Modela capacidad finita, latencia e intervalo de iniciación independientes.
 * Reserva el resultado al aceptar y la lane garantiza su recepción.
 * SoftFloat se aísla durante cada evaluación, sin callbacks en ese ámbito.
 * No accede al VRF ni al estado CPU, y no habilita nuevas instrucciones.
 * Propietario y receptor sobreviven a la FPU hasta completar drain.
 */
class Fpu
{
  public:
    using ResultSender = std::function<void(const ExecutionResult &)>;

    Fpu(ClockedObject &owner, LaneId lane_id, ResultSender send_result,
        statistics::Group &stats_parent,
        const ExecutionParameters &parameters);
    ~Fpu();

    Fpu(const Fpu &) = delete;
    Fpu &operator=(const Fpu &) = delete;

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
    statistics::Scalar inexactBundles;
    statistics::Scalar underflowBundles;
    statistics::Scalar overflowBundles;
    statistics::Scalar divideByZeroBundles;
    statistics::Scalar invalidBundles;
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

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__
