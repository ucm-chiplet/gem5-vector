// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__

#include <cstddef>
#include <deque>
#include <functional>
#include <string>

#include "cpu/vector_engine/common/execution_types.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/common/vpu_parameters.hh"
#include "cpu/vector_engine/vpu/register_file/vrf_port.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Una instancia de ALU entera de 8/16/32/64 bits: suma, multiplicación de
 * bits bajos, división y resto con y sin signo, formas .vv y .vx.
 *
 * Cada operación tiene su latencia e intervalo de iniciación (ciclos del
 * reloj VPU). Suma y multiplicación son segmentadas; división y resto usan
 * un divisor no segmentado y bloquean nuevas admisiones de esta instancia
 * hasta entregar su resultado. Las otras instancias y la FPU no dependen de
 * esa ocupación. La capacidad limita los bundles aceptados sin resultado.
 *
 * Como las latencias difieren por operación, los resultados se entregan
 * en cuanto están listos (por readyAt) y, a igualdad, en orden de admisión.
 * La lane los localiza por clave, no por orden de llegada.
 *
 * Copia los bundles aceptados y reserva sus resultados. La lane reserva
 * también recepción antes de enviar: el callback de resultado no se bloquea.
 * No accede al VRF ni completa instrucciones. Propietario y receptor
 * sobreviven al módulo hasta completar drain; no se cancela trabajo activo.
 */
class Alu
{
  public:
    using ResultSender = std::function<void(const ExecutionResult &)>;

    Alu(ClockedObject &owner, LaneId lane_id, std::size_t instance,
        ResultSender send_result, statistics::Group &stats_parent,
        const AluParameters &parameters);
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
    statistics::Scalar addElements;
    statistics::Scalar multiplyElements;
    statistics::Scalar divideElements;
    statistics::Scalar remainderElements;
    statistics::Scalar capacityStalls;
    statistics::Scalar intervalStalls;
    ClockedObject &owner;
    const AluParameters &parameters;
    ResultSender sendResult;
    // Orden de admisión; los resultados pueden salir antes por readyAt.
    std::deque<ExecutionState> pending;
    Tick nextIssue = 0;
    bool responding = false;
    EventFunctionWrapper executeEvent;

    /** Latencia e intervalo de una operación admitida con ese SEW. */
    OperationTiming timing(ArithmeticOperation operation,
                           uint16_t sew_bits) const;
    void countElements(const ExecutionBundle &bundle);
    void execute();
    void scheduleNextResult();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__
