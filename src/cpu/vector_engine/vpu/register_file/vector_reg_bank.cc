// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/register_file/vector_reg_bank.hh"

#include <algorithm>
#include <string>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

VectorRegBank::VectorRegBank(ClockedObject &owner, const AddressMapper &mapper,
                             LaneId lane_id, BankId bank_id,
                             ReadSender send_read,
                             WriteAckSender send_write_ack)
    : owner(owner),
      geometry(mapper.geometry()),
      laneId(lane_id),
      rowCount(mapper.rowsPerBank()),
      sendRead(std::move(send_read)),
      sendWriteAck(std::move(send_write_ack)),
      accessEvent([this] { execute(); }, owner.name() + ".lrf" +
                                             std::to_string(lane_id) +
                                             ".bank" + std::to_string(bank_id))
{
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
        return TransferResult::Retry;
    }
    active = PendingAccess{request, ByteBuffer(geometry.laneWordBytes, 0)};
    owner.schedule(accessEvent, owner.clockEdge(Cycles(1)));
    return TransferResult::Accepted;
}

TransferResult
VectorRegBank::write(const BankWriteRequest &request)
{
    validateAccess(request.key, request.row, request.wordByteEnable);
    panic_if(request.wordData.size() != geometry.laneWordBytes,
             "VRF bank write data must contain a complete word");
    if (active) {
        return TransferResult::Retry;
    }
    active = PendingAccess{request, {}};
    owner.schedule(accessEvent, owner.clockEdge(Cycles(1)));
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
        active.reset();
        // El ack confirma bytes aplicados; los deshabilitados se conservan.
        sendWriteAck(ack);
    }
}

} // namespace gem5::vector_engine
