// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_LANE_REGISTER_FILE_HH__
#define __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_LANE_REGISTER_FILE_HH__

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/vpu/register_file/vector_reg_bank.hh"

namespace gem5::vector_engine
{

/**
 * Slice local del VRF. Cada banco arbitra por ciclo entre puertos de entrada
 * persistentes. Los emisores poseen las peticiones no concedidas; el LRF
 * sólo conserva correlación y recepción de accesos aceptados. Sus entradas
 * de retorno y las del banco representan los mismos accesos, no dos colas.
 *
 * Los puertos se enlazan durante construcción y sus índices son estables.
 * Round-robin por banco/puerto físico garantiza oportunidades a lecturas y
 * writebacks compatibles. Un conflicto no adelanta a otro candidato anterior
 * según (ciclo de elegibilidad, índice de entrada). Las respuestas conservan
 * VrfAccessKey y pueden llegar en distinto orden entre bancos.
 *
 * Receptores, productores y mapper sobreviven al módulo hasta isIdle().
 * Antes de conceder existe recepción reservada en el emisor, LRF y banco.
 */
class LaneRegisterFile
{
  public:
    using ReadSender = std::function<void(const ReadResponse &)>;
    using WriteAckSender = std::function<void(const WriteAck &)>;
    struct ResponsePort
    {
        ReadSender sendRead;
        WriteAckSender sendWriteAck;
    };

    LaneRegisterFile(ClockedObject &owner, const AddressMapper &mapper,
                     const BankParameters &parameters, LaneId lane_id,
                     ResponsePort lane_responses, ResponsePort vlsu_responses);
    ~LaneRegisterFile();
    LaneRegisterFile(const LaneRegisterFile &) = delete;
    LaneRegisterFile &operator=(const LaneRegisterFile &) = delete;

    // Sólo construcción; no se pueden cambiar entradas durante ejecución.
    void bindRequestPort(VrfRequestPort port);
    // Un productor publica trabajo estable; compite desde el siguiente flanco.
    void wakeup();
    bool isIdle() const;

  private:
    enum class AccessKind
    {
        Read,
        Write
    };
    struct PendingAccess
    {
        VrfAccess access;
        MappedVrfFragment mapping;
        AccessKind kind;
        ByteBuffer readData;
    };
    struct Candidate
    {
        VrfOffer offer;
        MappedVrfFragment mapping;
        bool write;
        bool granted = false;
    };

    ModuleStats stats;
    statistics::Scalar reads;
    statistics::Scalar writes;
    statistics::Scalar readBytes;
    statistics::Scalar writeBytes;
    statistics::Scalar grantWaitTicks;
    statistics::Vector waitCycles;
    statistics::Vector capacityWaitCycles;
    statistics::Vector portWaitCycles;
    statistics::Vector conflictWaitCycles;
    ClockedObject &owner;
    const AddressMapper &mapper;
    const BankParameters &parameters;
    const LaneId laneId;
    ResponsePort laneResponses;
    ResponsePort vlsuResponses;
    std::vector<std::unique_ptr<VectorRegBank>> banks;
    std::vector<std::vector<std::optional<PendingAccess>>> pending;
    std::vector<VrfRequestPort> inputs;
    std::vector<std::vector<std::size_t>> nextInput;
    bool started = false;
    EventFunctionWrapper arbitrationEvent;

    bool hasOffers() const;
    MappedVrfFragment validateAccess(const VrfAccess &access) const;
    static bool conflicts(const Candidate &a, const Candidate &b);
    bool blockedByOlder(
        std::size_t input,
        const std::vector<std::optional<Candidate>> &candidates) const;
    void arbitrate();
    void grant(std::size_t input, std::size_t port, Candidate &candidate);
    std::optional<PendingAccess> &findPending(BankId bank,
                                              const VrfAccessKey &key);
    void recvBankReadResponse(BankId bank_id,
                              const BankReadResponse &response);
    void recvBankWriteAck(BankId bank_id, const BankWriteAck &ack);
    const ResponsePort &responsePort(const VrfRequester &requester) const;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_LANE_REGISTER_FILE_HH__
