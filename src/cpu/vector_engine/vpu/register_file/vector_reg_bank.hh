// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VECTOR_REG_BANK_HH__
#define __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VECTOR_REG_BANK_HH__

#include <deque>
#include <functional>
#include <variant>
#include <vector>

#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "cpu/vector_engine/vpu/register_file/vrf_port.hh"
#include "sim/clocked_object.hh"

namespace gem5::vector_engine
{

/**
 * Banco temporizado con puertos R/W/RW y accesos aceptados acotados.
 * El LRF arbitra las solicitudes; el banco sólo conserva operaciones ya
 * concedidas, con espacio para datos y retorno reservado antes de aceptar.
 * No contiene una FIFO de peticiones denegadas.
 *
 * Cada puerto tiene su intervalo de iniciación. La latencia fija determina
 * cuándo se leen/aplican los bytes y se emite la respuesta. Un RW sirve una
 * sola dirección por oportunidad. Las incompatibilidades de fila se revisan
 * antes de conceder, nunca se resuelven por el orden de los callbacks.
 * El propietario y los receptores sobreviven al banco hasta isIdle().
 */
class VectorRegBank
{
  public:
    using ReadSender = std::function<void(const BankReadResponse &)>;
    using WriteAckSender = std::function<void(const BankWriteAck &)>;

    VectorRegBank(ClockedObject &owner, const AddressMapper &mapper,
                  const BankParameters &parameters, LaneId lane_id,
                  BankId bank_id, ReadSender send_read,
                  WriteAckSender send_write_ack,
                  statistics::Group &stats_parent);
    ~VectorRegBank();

    VectorRegBank(const VectorRegBank &) = delete;
    VectorRegBank &operator=(const VectorRegBank &) = delete;

    std::size_t
    portCount() const
    {
        return ports.size();
    }
    bool hasCapacity() const;
    bool portReady(std::size_t port, bool write) const;
    bool conflicts(uint64_t row, const ByteEnable &enables, bool write) const;

    // Sólo el árbitro invoca estas funciones después de comprobar recursos.
    // Una concesión copia la petición y garantiza una única respuesta.
    void read(std::size_t port, const BankReadRequest &request);
    void write(std::size_t port, const BankWriteRequest &request);
    bool isIdle() const;

  private:
    enum class PortKind
    {
        Read,
        Write,
        ReadWrite
    };
    struct PortState
    {
        PortKind kind;
        Tick nextIssue = 0;
    };
    using BankRequest = std::variant<BankReadRequest, BankWriteRequest>;
    struct PendingAccess
    {
        BankRequest request;
        ByteBuffer readData;
        Tick readyAt;
    };

    ModuleStats stats;
    statistics::Scalar reads;
    statistics::Scalar writes;
    statistics::Scalar readBytes;
    statistics::Scalar writeBytes;
    statistics::Vector portReads;
    statistics::Vector portWrites;
    statistics::Vector portReadBytes;
    statistics::Vector portWriteBytes;
    statistics::Vector accessSizes;
    ClockedObject &owner;
    const VrfGeometry &geometry;
    const BankParameters &parameters;
    const LaneId laneId;
    const uint64_t rowCount;
    ReadSender sendRead;
    WriteAckSender sendWriteAck;
    ByteBuffer storage;
    std::vector<PortState> ports;
    std::deque<PendingAccess> pending;
    bool responding = false;
    EventFunctionWrapper accessEvent;

    void validateAccess(const VrfAccessKey &key, uint64_t row,
                        const ByteEnable &byte_enable) const;
    void accept(std::size_t port, BankRequest request, bool write);
    void execute();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_REGISTER_FILE_VECTOR_REG_BANK_HH__
