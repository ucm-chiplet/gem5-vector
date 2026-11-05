// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__

#include <cstdint>
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
 * FPU de una lane, dedicada sólo a coma flotante.
 *
 * Operaciones: FloatAdd binary16 (capacidad interna, sin offload FP16) y
 * FloatAdd, FloatMultiply y FloatDivide binary32, con el redondeo capturado
 * por CPU en cada bundle y los flags RVV agregados por bundle.
 *
 * Dos subrecursos independientes:
 * - Pipeline segmentado de suma/multiplicación, con parameters.unit.capacity
 *   entradas e intervalo de iniciación propio.
 * - Divisor no segmentado, con un bundle como máximo hasta su resultado.
 * Una división larga no bloquea el pipeline ni las ALUs. Los resultados se
 * entregan por readyAt; la lane los localiza por clave.
 *
 * SoftFloat se aísla durante cada evaluación, sin callbacks en ese ámbito.
 * No accede al VRF ni al estado CPU; Minor aplica los flags al retirar.
 * Propietario y receptor sobreviven a la FPU hasta completar drain.
 */
class Fpu
{
  public:
    using ResultSender = std::function<void(const ExecutionResult &)>;

    Fpu(ClockedObject &owner, LaneId lane_id, ResultSender send_result,
        statistics::Group &stats_parent, const FpuParameters &parameters);
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
        // Subrecurso ocupado: divisor o pipeline de suma/multiplicación.
        bool divider;
    };

    ModuleStats stats;
    statistics::Scalar add16Elements;
    statistics::Scalar add32Elements;
    statistics::Scalar multiply32Elements;
    statistics::Scalar divide32Elements;
    statistics::Scalar inexactBundles;
    statistics::Scalar underflowBundles;
    statistics::Scalar overflowBundles;
    statistics::Scalar divideByZeroBundles;
    statistics::Scalar invalidBundles;
    statistics::Scalar capacityStalls;
    statistics::Scalar intervalStalls;
    statistics::Scalar dividerStalls;
    ClockedObject &owner;
    const FpuParameters &parameters;
    ResultSender sendResult;
    // Orden de admisión; los resultados pueden salir antes por readyAt.
    std::deque<ExecutionState> pending;
    uint32_t pipelineEntries = 0;
    bool dividerBusy = false;
    Tick pipelineNextIssue = 0;
    bool responding = false;
    EventFunctionWrapper executeEvent;

    void validateBundle(const ExecutionBundle &bundle) const;
    void countElements(const ExecutionBundle &bundle);
    void compute(ExecutionState &state) const;
    void execute();
    void scheduleNextResult();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__
