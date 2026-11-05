// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__
#define __CPU_VECTOR_ENGINE_VPU_LANES_ARA_LANE_HH__

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cpu/vector_engine/common/lane_completion.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/common/operation_traits.hh"
#include "cpu/vector_engine/vpu/lanes/alu.hh"
#include "cpu/vector_engine/vpu/lanes/fpu.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "enums/ByteOrder.hh"

namespace gem5::vector_engine
{

/**
 * Lane con contextos acotados para fragmentos de tareas aritméticas.
 * Puede solapar lectura, cálculo y writeback de fragmentos de varias
 * instrucciones independientes; el scoreboard del sequencer garantiza que
 * no comparten registros con riesgo RAW, WAR ni WAW.
 *
 * Los contextos se ordenan por admisión. El distribuidor entrega en cada
 * lane las tareas en orden de inicio y los fragmentos de cada tarea en
 * orden, así que la admisión coincide con la antigüedad del fragmento.
 *
 * Cada fragmento pertenece a una clase de unidad (grupo de ALUs o FPU),
 * según la ficha de su operación. Cada clase tiene sus buffers de
 * operandos y resultados, como las colas por unidad de Ara: una división
 * FP retenida no ocupa los créditos de las ALUs. Reserva conjuntamente
 * ambos buffers de operandos de su clase antes de publicar las lecturas y
 * reserva un resultado antes de enviar a una unidad. El escalar capturado
 * ocupa su buffer replicado, pero no solicita un puerto de lectura al VRF.
 *
 * En cada ciclo se inicia como máximo un bundle por clase, en orden de
 * admisión dentro de la clase: un fragmento joven no adelanta a otro de su
 * clase que aún reúne operandos, pero sí a uno de la otra clase. Esto no
 * reordena el inicio de instrucciones, que sigue siendo FIFO en el
 * sequencer. Un bundle ALU va a la instancia de menor índice que lo acepte.
 * Contextos, puertos del VRF y writeback siguen siendo compartidos.
 *
 * Los accesos se presentan persistentemente al LRF por contexto y papel.
 * Cada retorno se localiza por su clave, nunca por orden de llegada.
 * Sólo WriteAck permite completar el fragmento y liberar su resultado.
 * Las fuentes de cada fragmento se leen antes de escribir su destino; los
 * rangos de otros fragmentos de la tarea son disjuntos, incluso con alias
 * legal de igual ancho. No se introduce chaining ni predicación.
 *
 * El accessId de cada acceso al VRF es fragmentId * 3 + papel, único
 * dentro de su TaskKey y de esta lane sin necesidad de contadores.
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
    /**
     * Buffers y esperas de una clase de unidad en esta lane. Los contadores
     * separan la espera de operandos (lectura), de resultados (writeback)
     * y los rechazos de la unidad (cálculo).
     */
    struct UnitClassState : public statistics::Group
    {
        UnitClassState(statistics::Group *parent, const std::string &name,
                       const ExecutionParameters &parameters);

        const ExecutionParameters &parameters;
        ModuleStats lhsBuffers;
        ModuleStats rhsBuffers;
        ModuleStats resultBuffers;
        statistics::Scalar operandWaitCycles;
        statistics::Scalar resultWaitCycles;
        statistics::Scalar executionRetries;
        uint32_t operandBundles = 0;
        uint32_t resultBundles = 0;
    };
    struct LaneFragmentState
    {
        LaneTask task;
        // Orden de admisión en esta lane; fija la prioridad de inicio.
        uint64_t admission = 0;
        // Clase de unidad que ejecuta el fragmento, fijada al admitirlo.
        ExecutionClass unit = ExecutionClass::IntegerAlu;
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
    // Phase accounting within the result reservation, not another resource.
    ModuleStats writebacks;
    statistics::Scalar vrfReads;
    statistics::Scalar vrfWrites;
    UnitClassState aluClass;
    UnitClassState fpuClass;
    ClockedObject &owner;
    const AddressMapper &mapper;
    const LaneParameters &parameters;
    const LaneId laneId;
    const ByteOrder targetByteOrder;
    VrfWakeup wakeVrf;
    CompletionSender sendCompletion;
    std::vector<std::optional<LaneFragmentState>> contexts;
    uint64_t nextAdmission = 0;
    // Instancias independientes; unique_ptr porque Alu no se puede mover.
    std::vector<std::unique_ptr<Alu>> alus;
    Fpu fpu;
    EventFunctionWrapper progressEvent;

    void validateTask(const LaneTask &task) const;
    static VrfAccess makeAccess(const LaneFragmentKey &fragment,
                                AccessRole role, LaneId lane,
                                const VectorRegRef &reg,
                                const ByteRange &range);
    std::vector<std::size_t> orderedContexts() const;
    UnitClassState &unitClass(ExecutionClass unit);
    void prepareOperands(LaneFragmentState &state);
    void issue(const std::vector<std::size_t> &order, ExecutionClass unit);
    void tryExecute(LaneFragmentState &state);
    TransferResult sendToUnit(const LaneFragmentState &state);
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
