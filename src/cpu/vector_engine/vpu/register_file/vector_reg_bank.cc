// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/register_file/vector_reg_bank.hh"

#include <algorithm>
#include <limits>
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
            "Granted read accesses"),
      writes(&stats, "writes", statistics::units::Count::get(),
             "Granted write accesses"),
      readBytes(&stats, "readBytes", statistics::units::Byte::get(),
                "Enabled bytes in granted reads"),
      writeBytes(&stats, "writeBytes", statistics::units::Byte::get(),
                 "Enabled bytes in granted writes"),
      portReads(&stats, "portReads", statistics::units::Count::get(),
                "Granted reads per physical port"),
      portWrites(&stats, "portWrites", statistics::units::Count::get(),
                 "Granted writes per physical port"),
      portReadBytes(&stats, "portReadBytes", statistics::units::Byte::get(),
                    "Enabled read bytes per physical port"),
      portWriteBytes(&stats, "portWriteBytes", statistics::units::Byte::get(),
                     "Enabled write bytes per physical port"),
      accessSizes(&stats, "accessSizes", statistics::units::Count::get(),
                  "Granted accesses indexed by enabled byte count"),
      owner(owner),
      geometry(mapper.geometry()),
      parameters(parameters),
      laneId(lane_id),
      rowCount(mapper.rowsPerBank()),
      sendRead(std::move(send_read)),
      sendWriteAck(std::move(send_write_ack)),
      accessEvent([this] { execute(); },
                  owner.name() + ".lrf" + std::to_string(lane_id) + ".bank" +
                      std::to_string(bank_id),
                  false, VpuResponsePriority)
{
    fatal_if(!parameters.currentProfile(), "Invalid timed VRF bank profile");
    fatal_if(parameters.accessEntries > pending.max_size(),
             "VRF access capacity exceeds host container capacity");
    vpuClockEdge(owner, parameters.latency);
    vpuClockEdge(owner, parameters.initiationInterval);
    fatal_if(laneId >= geometry.numLanes || bank_id >= geometry.banksPerLane,
             "VRF bank identity is outside its geometry");
    fatal_if(!sendRead || !sendWriteAck,
             "VRF bank requires response receivers");
    fatal_if(!rowCount ||
                 rowCount > storage.max_size() / geometry.laneWordBytes,
             "VRF bank storage exceeds host container capacity");
    const uint64_t port_count = uint64_t{parameters.readPorts} +
                                parameters.writePorts +
                                parameters.readWritePorts;
    fatal_if(port_count > ports.max_size() ||
                 port_count > std::numeric_limits<int>::max() ||
                 uint64_t{geometry.laneWordBytes} + 1 >
                     std::numeric_limits<int>::max(),
             "VRF bank port/width statistics exceed representable sizes");
    storage.resize(static_cast<std::size_t>(rowCount) * geometry.laneWordBytes,
                   0);
    // Dedicated ports precede RW ports in the arbiter's deterministic pass.
    ports.insert(ports.end(), parameters.readPorts, {PortKind::Read, 0});
    ports.insert(ports.end(), parameters.writePorts, {PortKind::Write, 0});
    ports.insert(ports.end(), parameters.readWritePorts,
                 {PortKind::ReadWrite, 0});
    portReads.init(port_count);
    portWrites.init(port_count);
    portReadBytes.init(port_count);
    portWriteBytes.init(port_count);
    accessSizes.init(geometry.laneWordBytes + 1);
    for (std::size_t i = 0; i < ports.size(); ++i) {
        std::string label;
        if (ports[i].kind == PortKind::Read) {
            label = "r" + std::to_string(i);
        } else if (ports[i].kind == PortKind::Write) {
            label = "w" + std::to_string(i - parameters.readPorts);
        } else {
            label = "rw" + std::to_string(i - parameters.readPorts -
                                          parameters.writePorts);
        }
        portReads.subname(i, label);
        portWrites.subname(i, label);
        portReadBytes.subname(i, label);
        portWriteBytes.subname(i, label);
    }
}

VectorRegBank::~VectorRegBank()
{
    panic_if(!isIdle(), "Destroying VRF bank before its work has drained");
}

bool
VectorRegBank::isIdle() const
{
    // A future nextIssue date without pending work is not a drain obligation.
    return pending.empty() && !responding && !accessEvent.scheduled();
}

bool
VectorRegBank::hasCapacity() const
{
    return pending.size() < parameters.accessEntries;
}

bool
VectorRegBank::portReady(std::size_t port, bool write) const
{
    panic_if(port >= ports.size(), "Unknown VRF bank port");
    const auto &state = ports[port];
    return state.nextIssue <= curTick() &&
           (state.kind == PortKind::ReadWrite ||
            (write && state.kind == PortKind::Write) ||
            (!write && state.kind == PortKind::Read));
}

bool
VectorRegBank::conflicts(uint64_t row, const ByteEnable &enables,
                         bool write) const
{
    for (const auto &entry : pending) {
        const bool pending_write =
            std::holds_alternative<BankWriteRequest>(entry.request);
        const bool conflict = std::visit(
            [&](const auto &request) {
                if (request.row != row || (!write && !pending_write)) {
                    return false;
                }
                if (write != pending_write) {
                    return true; // R/W of one row wait for the older response.
                }
                for (std::size_t i = 0; i < enables.size(); ++i) {
                    if (enables[i] && request.wordByteEnable[i]) {
                        return true;
                    }
                }
                return false;
            },
            entry.request);
        if (conflict) {
            return true;
        }
    }
    return false;
}

void
VectorRegBank::validateAccess(const VrfAccessKey &key, uint64_t row,
                              const ByteEnable &byte_enable) const
{
    panic_if(!key.valid() || (key.requester.kind == VrfRequesterKind::Lane &&
                              *key.requester.laneId != laneId),
             "Invalid identity in a VRF bank access");
    panic_if(row >= rowCount || byte_enable.size() != geometry.laneWordBytes,
             "VRF bank row or byte enables are outside its geometry");
    panic_if(std::any_of(byte_enable.begin(), byte_enable.end(),
                         [](uint8_t enable) { return enable > 1; }),
             "VRF bank byte enables must be zero or one");
    for (const auto &entry : pending) {
        panic_if(
            std::visit([&](const auto &request) { return request.key == key; },
                       entry.request),
            "Duplicate granted VRF bank access");
    }
}

void
VectorRegBank::read(std::size_t port, const BankReadRequest &request)
{
    validateAccess(request.key, request.row, request.wordByteEnable);
    accept(port, request, false);
}

void
VectorRegBank::write(std::size_t port, const BankWriteRequest &request)
{
    validateAccess(request.key, request.row, request.wordByteEnable);
    panic_if(request.wordData.size() != geometry.laneWordBytes,
             "VRF bank write data must contain a complete word");
    accept(port, request, true);
}

void
VectorRegBank::accept(std::size_t port, BankRequest request, bool write)
{
    const auto &enables = std::visit(
        [](const auto &value) -> const ByteEnable & {
            return value.wordByteEnable;
        },
        request);
    const auto row =
        std::visit([](const auto &value) { return value.row; }, request);
    panic_if(!hasCapacity() || !portReady(port, write) ||
                 conflicts(row, enables, write),
             "VRF arbiter granted an unavailable bank resource");
    const auto bytes = std::count(enables.begin(), enables.end(), 1);
    const auto ready = vpuClockEdge(owner, parameters.latency);
    panic_if(ready <= curTick(), "VRF response time overflow");
    ByteBuffer read_data;
    if (!write) {
        read_data.resize(geometry.laneWordBytes, 0);
    }
    pending.push_back({std::move(request), std::move(read_data), ready});
    ports[port].nextIssue = vpuClockEdge(owner, parameters.initiationInterval);
    panic_if(ports[port].nextIssue <= curTick(), "VRF issue time overflow");
    stats.accept(0, bytes);
    ++accessSizes[bytes];
    if (write) {
        ++writes;
        writeBytes += bytes;
        ++portWrites[port];
        portWriteBytes[port] += bytes;
    } else {
        ++reads;
        readBytes += bytes;
        ++portReads[port];
        portReadBytes[port] += bytes;
    }
    if (!responding && !accessEvent.scheduled()) {
        owner.schedule(accessEvent, pending.front().readyAt);
    }
}

void
VectorRegBank::execute()
{
    // Fixed latency preserves grant order within a bank. Release each entry
    // before its callback; reception is pre-reserved and cannot reject it.
    responding = true;
    while (!pending.empty() && pending.front().readyAt <= curTick()) {
        auto entry = std::move(pending.front());
        pending.pop_front();
        stats.complete();
        if (const auto *request =
                std::get_if<BankReadRequest>(&entry.request)) {
            BankReadResponse response{request->key, std::move(entry.readData)};
            const auto offset = static_cast<std::size_t>(request->row) *
                                geometry.laneWordBytes;
            for (std::size_t i = 0; i < response.wordData.size(); ++i) {
                if (request->wordByteEnable[i]) {
                    response.wordData[i] = storage[offset + i];
                }
            }
            sendRead(response);
        } else {
            const auto &write = std::get<BankWriteRequest>(entry.request);
            const auto offset =
                static_cast<std::size_t>(write.row) * geometry.laneWordBytes;
            for (std::size_t i = 0; i < write.wordData.size(); ++i) {
                if (write.wordByteEnable[i]) {
                    storage[offset + i] = write.wordData[i];
                }
            }
            sendWriteAck(BankWriteAck{write.key});
        }
    }
    responding = false;
    if (!pending.empty() && !accessEvent.scheduled()) {
        owner.schedule(accessEvent, pending.front().readyAt);
    }
}

} // namespace gem5::vector_engine
