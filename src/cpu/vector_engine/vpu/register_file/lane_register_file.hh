// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_LANE_REGISTER_FILE_HH__
#define __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_LANE_REGISTER_FILE_HH__

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cpu/vector_engine/vpu/register_file/vector_reg_bank.hh"

namespace gem5::vector_engine
{

/**
 * Slice local del VRF, propietaria de banksPerLane bancos independientes.
 * Concede por orden de llegada un acceso por banco; la contención devuelve
 * Retry sin retener la solicitud ni alterar el almacenamiento. Las entradas
 * locales sólo conservan el retorno de operaciones aceptadas, no una FIFO.
 *
 * El propietario conecta los puertos de respuesta a la lane local y a la
 * VLSU. Cada emisor reserva espacio de recepción antes de llamar read/write.
 * Accepted obliga a una respuesta única; ésta conserva la clave original.
 * Las respuestas pueden admitir accesos nuevos, pero no destruir el LRF.
 *
 * Propietario, mapper y receptores sobreviven al módulo. Drain termina el
 * trabajo aceptado; sólo se destruye cuando isIdle(). La capacidad efectiva
 * según LMUL y el rango activo se validan antes de emitir accesos al VRF.
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
                     LaneId lane_id, ResponsePort lane_responses,
                     ResponsePort vlsu_responses);
    ~LaneRegisterFile();

    LaneRegisterFile(const LaneRegisterFile &) = delete;
    LaneRegisterFile &operator=(const LaneRegisterFile &) = delete;

    TransferResult read(const VrfReadRequest &request);
    TransferResult write(const VrfWriteRequest &request);
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
        // Reservado antes de conceder una lectura; vacío para escrituras.
        ByteBuffer readData;
    };

    const AddressMapper &mapper;
    const LaneId laneId;
    ResponsePort laneResponses;
    ResponsePort vlsuResponses;
    std::vector<std::unique_ptr<VectorRegBank>> banks;
    std::vector<std::optional<PendingAccess>> pending;

    MappedVrfFragment validateAccess(const VrfAccess &access) const;
    void handleTransfer(BankId bank_id, TransferResult result);
    void recvBankReadResponse(BankId bank_id,
                              const BankReadResponse &response);
    void recvBankWriteAck(BankId bank_id, const BankWriteAck &ack);
    const ResponsePort &responsePort(const VrfRequester &requester) const;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_LANE_REGISTER_FILE_HH__
