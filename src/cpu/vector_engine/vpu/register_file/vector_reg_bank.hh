// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VECTOR_REG_BANK_HH__
#define __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VECTOR_REG_BANK_HH__

#include <functional>
#include <optional>
#include <variant>

#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Banco local con un acceso en curso, compartido entre lectura y escritura.
 * Recibe palabras ya mapeadas desde su LRF; no contiene FIFO ni resuelve
 * referencias a registros arquitectónicos. Copia las peticiones aceptadas
 * y reserva el buffer de lectura antes de conceder el acceso.
 *
 * La operación y su respuesta se ejecutan en el siguiente flanco del reloj
 * del propietario. Esta demora funcional no fija la latencia hardware Ara.
 * El almacenamiento empieza a cero; no ofrece reset ni cancelación activa.
 * Propietario, geometría y receptor sobreviven al banco hasta isIdle(). Los
 * callbacks pueden admitir trabajo nuevo, pero no destruir el módulo.
 */
class VectorRegBank
{
  public:
    using ReadSender = std::function<void(const BankReadResponse &)>;
    using WriteAckSender = std::function<void(const BankWriteAck &)>;

    VectorRegBank(ClockedObject &owner, const AddressMapper &mapper,
                  LaneId lane_id, BankId bank_id, ReadSender send_read,
                  WriteAckSender send_write_ack);
    ~VectorRegBank();

    VectorRegBank(const VectorRegBank &) = delete;
    VectorRegBank &operator=(const VectorRegBank &) = delete;

    TransferResult read(const BankReadRequest &request);
    TransferResult write(const BankWriteRequest &request);

    bool
    isIdle() const
    {
        return !active && !accessEvent.scheduled();
    }

  private:
    struct PendingAccess
    {
        std::variant<BankReadRequest, BankWriteRequest> request;
        ByteBuffer readData;
    };

    ClockedObject &owner;
    const VrfGeometry &geometry;
    const LaneId laneId;
    const uint64_t rowCount;
    ReadSender sendRead;
    WriteAckSender sendWriteAck;
    ByteBuffer storage;
    std::optional<PendingAccess> active;
    EventFunctionWrapper accessEvent;

    void validateAccess(const VrfAccessKey &key, uint64_t row,
                        const ByteEnable &byte_enable) const;
    void execute();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VECTOR_REG_BANK_HH__
