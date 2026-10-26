// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/register_file/vector_reg_bank.hh"

#include <algorithm>
#include <string>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

VectorRegBank::VectorRegBank(ClockedObject &owner, const AddressMapper &mapper,
                             const BankParameters &parameters, LaneId lane_id,
                             BankId bank_id, ReadSender send_read,
                             WriteAckSender send_write_ack,
                             statistics::Group &stats_parent)
    : stats(&stats_parent, "bank" + std::to_string(bank_id),
            "VRF word accesses"),
      reads(&stats, "reads", statistics::units::Count::get(),
            "Accepted read accesses"),
      writes(&stats, "writes", statistics::units::Count::get(),
             "Accepted write accesses"),
      readBytes(&stats, "readBytes", statistics::units::Byte::get(),
                "Enabled bytes in accepted read accesses"),
      writeBytes(&stats, "writeBytes", statistics::units::Byte::get(),
                 "Enabled bytes in accepted write accesses"),
      owner(owner),
      geometry(mapper.geometry()),
      parameters(parameters),
      laneId(lane_id),
      rowCount(mapper.rowsPerBank()),
      sendRead(std::move(send_read)),
      sendWriteAck(std::move(send_write_ack)),
      accessEvent([this] { execute(); }, owner.name() + ".lrf" +
                                             std::to_string(lane_id) +
                                             ".bank" + std::to_string(bank_id))
{
    fatal_if(!parameters.currentProfile(),
             "VRF bank supports one pending access and one RW port");
    fatal_if(laneId >= geometry.numLanes || bank_id >= geometry.banksPerLane,
             "VRF bank identity is outside its geometry");
    fatal_if(!sendRead || !sendWriteAck,
             "VRF bank requires read and write acknowledgement endpoints");
    fatal_if(!rowCount ||
                 rowCount > storage.max_size() / geometry.laneWordBytes,
             "VRF bank storage exceeds the host container capacity");
    storage.resize(static_cast<std::size_t>(rowCount) * geometry.laneWordBytes,
                   0);
}

VectorRegBank::~VectorRegBank()
{
    panic_if(!isIdle(), "Destroying VRF bank before its work has drained");
}

void
VectorRegBank::validateAccess(const VrfAccessKey &key, uint64_t row,
                              const ByteEnable &byte_enable) const
{
    panic_if(!key.valid() || (key.requester.kind == VrfRequesterKind::Lane &&
                              *key.requester.laneId != laneId),
             "Invalid requester or identity in a VRF bank access");
    panic_if(row >= rowCount || byte_enable.size() != geometry.laneWordBytes,
             "VRF bank row or word mask is outside its geometry");
    panic_if(std::any_of(byte_enable.begin(), byte_enable.end(),
                         [](uint8_t enable) { return enable > 1; }),
             "VRF bank byte enables must be zero or one");
    panic_if(
        active &&
            std::visit([&](const auto &request) { return request.key == key; },
                       active->request),
        "Duplicate accepted VRF bank access");
}

TransferResult
VectorRegBank::read(const BankReadRequest &request)
{
    validateAccess(request.key, request.row, request.wordByteEnable);
    if (active) {
        ++stats.retries;
        return TransferResult::Retry;
    }
    active = PendingAccess{request, ByteBuffer(geometry.laneWordBytes, 0)};
    ++reads;
    readBytes += std::count(request.wordByteEnable.begin(),
                            request.wordByteEnable.end(), 1);
    stats.accept(0, std::count(request.wordByteEnable.begin(),
                               request.wordByteEnable.end(), 1));
    owner.schedule(accessEvent, owner.clockEdge(parameters.latency));
    return TransferResult::Accepted;
}

TransferResult
VectorRegBank::write(const BankWriteRequest &request)
{
    validateAccess(request.key, request.row, request.wordByteEnable);
    panic_if(request.wordData.size() != geometry.laneWordBytes,
             "VRF bank write data must contain a complete word");
    if (active) {
        ++stats.retries;
        return TransferResult::Retry;
    }
    active = PendingAccess{request, {}};
    ++writes;
    writeBytes += std::count(request.wordByteEnable.begin(),
                             request.wordByteEnable.end(), 1);
    stats.accept(0, std::count(request.wordByteEnable.begin(),
                               request.wordByteEnable.end(), 1));
    owner.schedule(accessEvent, owner.clockEdge(parameters.latency));
    return TransferResult::Accepted;
}

void
VectorRegBank::execute()
{
    panic_if(!active, "VRF bank evaluation without an accepted access");
    if (const auto *request = std::get_if<BankReadRequest>(&active->request)) {
        BankReadResponse response{request->key, std::move(active->readData)};
        const auto offset =
            static_cast<std::size_t>(request->row) * geometry.laneWordBytes;
        for (std::size_t i = 0; i < response.wordData.size(); ++i) {
            if (request->wordByteEnable[i]) {
                response.wordData[i] = storage[offset + i];
            }
        }
        // Se libera antes del callback, que puede admitir el siguiente acceso.
        stats.complete();
        active.reset();
        sendRead(response);
    } else {
        const auto &write_request =
            std::get<BankWriteRequest>(active->request);
        const auto offset = static_cast<std::size_t>(write_request.row) *
                            geometry.laneWordBytes;
        for (std::size_t i = 0; i < write_request.wordData.size(); ++i) {
            if (write_request.wordByteEnable[i]) {
                storage[offset + i] = write_request.wordData[i];
            }
        }
        const BankWriteAck ack{write_request.key};
        stats.complete();
        active.reset();
        // El ack confirma bytes aplicados; los deshabilitados se conservan.
        sendWriteAck(ack);
    }
}

} // namespace gem5::vector_engine
