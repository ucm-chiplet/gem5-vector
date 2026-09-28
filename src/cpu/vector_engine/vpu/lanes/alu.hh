// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__

#include <functional>
#include <optional>
#include <vector>

#include "cpu/vector_engine/common/lane_task.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

struct ExecutionBundle
{
    LaneFragmentKey key;
    ArithmeticOperation operation = ArithmeticOperation::Invalid;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    std::vector<uint32_t> lhs;
    std::vector<uint32_t> rhs;
};

struct ExecutionResult
{
    LaneFragmentKey key;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    std::vector<uint32_t> values;
};

/**
 * ALU funcional de 32 bits con capacidad para un bundle. Copia los operandos
 * y reserva el resultado al aceptar; responde en una evaluación posterior.
 * El evento garantiza progreso, no modela una latencia de hardware Ara.
 * El propietario y el receptor sobreviven al módulo, hasta completar drain.
 */
class Alu
{
  public:
    using ResultSender = std::function<void(const ExecutionResult &)>;

    Alu(ClockedObject &owner, LaneId lane_id, ResultSender send_result);
    ~Alu();

    Alu(const Alu &) = delete;
    Alu &operator=(const Alu &) = delete;

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

#endif // __CPU_VECTOR_ENGINE_VPU_LANES_ALU_HH__
