// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__

#include <functional>
#include <optional>

#include "cpu/vector_engine/common/execution_types.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * FPU funcional binary16: FloatAdd con redondeo explícito y flags acumulados.
 * Acepta un bundle, reserva el resultado y responde en un evento posterior.
 * El evento garantiza progreso; no representa la latencia hardware de Ara.
 * No accede al VRF, al estado CPU ni a otros módulos de control.
 * El propietario y el receptor sobreviven a la FPU hasta completar drain.
 */
class Fpu
{
  public:
    using ResultSender = std::function<void(const ExecutionResult &)>;

    Fpu(ClockedObject &owner, LaneId lane_id, ResultSender send_result);
    ~Fpu();

    Fpu(const Fpu &) = delete;
    Fpu &operator=(const Fpu &) = delete;

    TransferResult acceptBundle(const ExecutionBundle &bundle);

    bool
    isIdle() const
    {
        return !active && !executeEvent.scheduled();
    }

  private:
    struct ExecutionState
    {
        ExecutionBundle bundle;
        ExecutionResult result;
    };

    ClockedObject &owner;
    ResultSender sendResult;
    std::optional<ExecutionState> active;
    EventFunctionWrapper executeEvent;

    void execute();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_FPU_HH__
