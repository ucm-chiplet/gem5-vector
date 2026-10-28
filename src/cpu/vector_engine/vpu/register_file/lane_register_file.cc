// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/register_file/lane_register_file.hh"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

LaneRegisterFile::LaneRegisterFile(ClockedObject &owner,
                                   const AddressMapper &mapper,
                                   const BankParameters &parameters,
                                   LaneId lane_id, ResponsePort lane_responses,
                                   ResponsePort vlsu_responses)
    : stats(&owner, "lrf" + std::to_string(lane_id), "VRF accesses"),
      reads(&stats, "reads", statistics::units::Count::get(),
            "Granted operand and VLSU reads"),
      writes(&stats, "writes", statistics::units::Count::get(),
             "Granted lane and VLSU writes"),
      readBytes(&stats, "readBytes", statistics::units::Byte::get(),
                "Enabled bytes in granted reads"),
      writeBytes(&stats, "writeBytes", statistics::units::Byte::get(),
                 "Enabled bytes in granted writes"),
      grantWaitTicks(&stats, "grantWaitTicks", statistics::units::Tick::get(),
                     "Sum of publication-to-grant ticks; excludes responses"),
      waitCycles(&stats, "waitCycles", statistics::units::Cycle::get(),
                 "Ungranted eligible request-cycles per bank"),
      capacityWaitCycles(&stats, "capacityWaitCycles",
                         statistics::units::Cycle::get(),
                         "Request-cycles without bank entry; may overlap"),
      portWaitCycles(&stats, "portWaitCycles", statistics::units::Cycle::get(),
                     "Request-cycles without ready compatible port; overlap"),
      conflictWaitCycles(&stats, "conflictWaitCycles",
                         statistics::units::Cycle::get(),
                         "Request-cycles with row/order conflict; overlap"),
      owner(owner),
      mapper(mapper),
      parameters(parameters),
      laneId(lane_id),
      laneResponses(std::move(lane_responses)),
      vlsuResponses(std::move(vlsu_responses)),
      arbitrationEvent([this] { arbitrate(); },
                       owner.name() + ".lrf" + std::to_string(lane_id) +
                           ".arbitrate",
                       false, VrfArbitrationPriority)
{
    fatal_if(!parameters.currentProfile(), "Invalid timed VRF bank profile");
    const auto &geometry = mapper.geometry();
    fatal_if(laneId >= geometry.numLanes, "LRF identity outside VRF geometry");
    fatal_if(!laneResponses.sendRead || !laneResponses.sendWriteAck ||
                 !vlsuResponses.sendRead || !vlsuResponses.sendWriteAck,
             "LRF requires read and write acknowledgement endpoints");
    fatal_if(geometry.banksPerLane > std::numeric_limits<int>::max(),
             "LRF bank statistics exceed representable size");
    pending.resize(geometry.banksPerLane);
    nextInput.resize(geometry.banksPerLane);
    for (BankId bank = 0; bank < geometry.banksPerLane; ++bank) {
        fatal_if(parameters.accessEntries > pending[bank].max_size(),
                 "LRF return entries exceed host container capacity");
        banks.push_back(std::make_unique<VectorRegBank>(
            owner, mapper, parameters, laneId, bank,
            [this, bank](const BankReadResponse &reply) {
                recvBankReadResponse(bank, reply);
            },
            [this, bank](const BankWriteAck &ack) {
                recvBankWriteAck(bank, ack);
            },
            stats));
        pending[bank].resize(parameters.accessEntries);
        nextInput[bank].resize(banks[bank]->portCount(), 0);
    }
    waitCycles.init(geometry.banksPerLane);
    capacityWaitCycles.init(geometry.banksPerLane);
    portWaitCycles.init(geometry.banksPerLane);
    conflictWaitCycles.init(geometry.banksPerLane);
}

LaneRegisterFile::~LaneRegisterFile()
{
    panic_if(!isIdle(), "Destroying LRF before its work has drained");
}

void
LaneRegisterFile::bindRequestPort(VrfRequestPort port)
{
    fatal_if(started || !port.peek || !port.granted,
             "LRF ports must be bound before execution with valid callbacks");
    inputs.push_back(std::move(port));
}

bool
LaneRegisterFile::hasOffers() const
{
    return std::any_of(inputs.begin(), inputs.end(), [](const auto &port) {
        return port.peek().has_value();
    });
}

bool
LaneRegisterFile::isIdle() const
{
    return !arbitrationEvent.scheduled() && !hasOffers() &&
           std::all_of(banks.begin(), banks.end(),
                       [](const auto &bank) { return bank->isIdle(); }) &&
           std::all_of(pending.begin(), pending.end(), [](const auto &b) {
               return std::none_of(b.begin(), b.end(), [](const auto &entry) {
                   return entry.has_value();
               });
           });
}

void
LaneRegisterFile::wakeup()
{
    started = true;
    if (!arbitrationEvent.scheduled()) {
        owner.schedule(arbitrationEvent, vpuClockEdge(owner, Cycles(1)));
    }
}

MappedVrfFragment
LaneRegisterFile::validateAccess(const VrfAccess &access) const
{
    panic_if(!access.key.valid() ||
                 (access.key.requester.kind == VrfRequesterKind::Lane &&
                  *access.key.requester.laneId != laneId),
             "Invalid requester or identity in an LRF access");
    panic_if(!access.range.valid() ||
                 access.range.size > mapper.geometry().laneWordBytes,
             "LRF access must fit in one word");
    auto mapping = mapper.map(access.reg, access.range, access.byteEnable);
    panic_if(mapping.size() != 1 || mapping.front().laneId != laneId ||
                 mapping.front().originalRange != access.range,
             "LRF access does not belong to one local VRF word");
    for (const auto &bank : pending) {
        for (const auto &entry : bank) {
            panic_if(entry && entry->access.key == access.key,
                     "Duplicate granted LRF access");
        }
    }
    return std::move(mapping.front());
}

bool
LaneRegisterFile::conflicts(const Candidate &a, const Candidate &b)
{
    if (a.mapping.bankId != b.mapping.bankId ||
        a.mapping.row != b.mapping.row || (!a.write && !b.write)) {
        return false;
    }
    if (a.write != b.write) {
        return true;
    }
    for (std::size_t i = 0; i < a.mapping.wordByteEnable.size(); ++i) {
        if (a.mapping.wordByteEnable[i] && b.mapping.wordByteEnable[i]) {
            return true;
        }
    }
    return false;
}

bool
LaneRegisterFile::blockedByOlder(
    std::size_t input,
    const std::vector<std::optional<Candidate>> &candidates) const
{
    const auto &current = *candidates[input];
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i] || candidates[i]->granted || i == input) {
            continue;
        }
        const auto &older = *candidates[i];
        const bool precedes =
            older.offer.eligibleAt < current.offer.eligibleAt ||
            (older.offer.eligibleAt == current.offer.eligibleAt && i < input);
        if (precedes && conflicts(older, current)) {
            return true;
        }
    }
    return false;
}

void
LaneRegisterFile::arbitrate()
{
    // Snapshot all eligible inputs before granting any. Producers expose
    // stable descriptors and stamp new offers for the following edge.
    std::vector<std::optional<Candidate>> candidates(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto offer = inputs[i].peek();
        if (!offer) {
            continue;
        }
        panic_if(!offer->request, "Null request in a persistent VRF offer");
        panic_if(offer->publishedAt >= offer->eligibleAt,
                 "VRF offer must become eligible after publication");
        if (offer->eligibleAt > curTick()) {
            continue;
        }
        const auto &access = vrfAccess(*offer->request);
        auto mapping = validateAccess(access);
        for (std::size_t j = 0; j < i; ++j) {
            panic_if(candidates[j] &&
                         vrfAccess(*candidates[j]->offer.request).key ==
                             access.key,
                     "Duplicate offered LRF access");
        }
        if (const auto *write = std::get_if<VrfWriteRequest>(offer->request)) {
            panic_if(write->data.size() != access.range.size,
                     "LRF write data does not match its byte range");
        }
        candidates[i] =
            Candidate{*offer, std::move(mapping), isVrfWrite(*offer->request)};
    }
    for (BankId bank = 0; bank < banks.size(); ++bank) {
        auto &resource = *banks[bank];
        for (std::size_t port = 0; port < resource.portCount(); ++port) {
            const auto start = nextInput[bank][port];
            for (std::size_t n = 0; n < candidates.size(); ++n) {
                const auto i = (start + n) % candidates.size();
                if (!candidates[i] || candidates[i]->granted) {
                    continue;
                }
                auto &candidate = *candidates[i];
                if (candidate.mapping.bankId != bank ||
                    !resource.hasCapacity() ||
                    !resource.portReady(port, candidate.write) ||
                    resource.conflicts(candidate.mapping.row,
                                       candidate.mapping.wordByteEnable,
                                       candidate.write) ||
                    blockedByOlder(i, candidates)) {
                    continue;
                }
                grant(i, port, candidate);
                nextInput[bank][port] = (i + 1) % candidates.size();
                break;
            }
        }
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i] || candidates[i]->granted) {
            continue;
        }
        const auto &candidate = *candidates[i];
        const auto bank = candidate.mapping.bankId;
        const auto &resource = *banks[bank];
        ++waitCycles[bank];
        ++stats.retries;
        if (!resource.hasCapacity()) {
            ++capacityWaitCycles[bank];
        }
        bool ready_port = false;
        for (std::size_t port = 0; port < resource.portCount(); ++port) {
            ready_port |= resource.portReady(port, candidate.write);
        }
        if (!ready_port) {
            ++portWaitCycles[bank];
        }
        if (resource.conflicts(candidate.mapping.row,
                               candidate.mapping.wordByteEnable,
                               candidate.write) ||
            blockedByOlder(i, candidates)) {
            ++conflictWaitCycles[bank];
        }
    }
    if (hasOffers()) {
        wakeup();
    }
}

void
LaneRegisterFile::grant(std::size_t input, std::size_t port,
                        Candidate &candidate)
{
    // Copy before granted() lets the producer change its presentation state.
    const auto request = *candidate.offer.request;
    const auto &access = vrfAccess(request);
    const auto bank = candidate.mapping.bankId;
    auto free = std::find_if(pending[bank].begin(), pending[bank].end(),
                             [](const auto &entry) { return !entry; });
    panic_if(free == pending[bank].end(), "Missing reserved LRF return entry");
    ByteBuffer read_data;
    if (!candidate.write) {
        read_data.resize(access.range.size, 0);
    }
    *free =
        PendingAccess{access, candidate.mapping,
                      candidate.write ? AccessKind::Write : AccessKind::Read,
                      std::move(read_data)};
    const auto bytes =
        std::count(access.byteEnable.begin(), access.byteEnable.end(), 1);
    stats.accept(0, bytes);
    grantWaitTicks += curTick() - candidate.offer.publishedAt;
    candidate.granted = true;
    inputs[input].granted(access.key);
    if (candidate.write) {
        const auto &write = std::get<VrfWriteRequest>(request);
        ByteBuffer word(mapper.geometry().laneWordBytes, 0);
        std::copy(write.data.begin(), write.data.end(),
                  word.begin() + candidate.mapping.byteOffsetInWord);
        ++writes;
        writeBytes += bytes;
        banks[bank]->write(port,
                           BankWriteRequest{access.key, candidate.mapping.row,
                                            candidate.mapping.wordByteEnable,
                                            std::move(word)});
    } else {
        ++reads;
        readBytes += bytes;
        banks[bank]->read(port,
                          BankReadRequest{access.key, candidate.mapping.row,
                                          candidate.mapping.wordByteEnable});
    }
}

std::optional<LaneRegisterFile::PendingAccess> &
LaneRegisterFile::findPending(BankId bank, const VrfAccessKey &key)
{
    panic_if(bank >= pending.size(), "Response from unknown VRF bank");
    auto match = std::find_if(
        pending[bank].begin(), pending[bank].end(),
        [&](const auto &entry) { return entry && entry->access.key == key; });
    panic_if(match == pending[bank].end(), "Unknown/duplicate LRF response");
    return *match;
}

const LaneRegisterFile::ResponsePort &
LaneRegisterFile::responsePort(const VrfRequester &requester) const
{
    panic_if(!requester.valid() || (requester.kind == VrfRequesterKind::Lane &&
                                    *requester.laneId != laneId),
             "Invalid requester in an LRF response");
    if (requester.kind == VrfRequesterKind::Lane) {
        return laneResponses;
    }
    return vlsuResponses;
}

void
LaneRegisterFile::recvBankReadResponse(BankId bank,
                                       const BankReadResponse &response)
{
    auto &slot = findPending(bank, response.key);
    panic_if(slot->kind != AccessKind::Read || slot->mapping.bankId != bank ||
                 response.wordData.size() != mapper.geometry().laneWordBytes,
             "VRF read response has wrong direction or size");
    ReadResponse reply{response.key, std::move(slot->readData)};
    std::copy_n(response.wordData.begin() + slot->mapping.byteOffsetInWord,
                reply.data.size(), reply.data.begin());
    stats.complete();
    slot.reset();
    responsePort(reply.key.requester).sendRead(reply);
}

void
LaneRegisterFile::recvBankWriteAck(BankId bank, const BankWriteAck &ack)
{
    auto &slot = findPending(bank, ack.key);
    panic_if(slot->kind != AccessKind::Write || slot->mapping.bankId != bank,
             "VRF write acknowledgement has wrong direction or bank");
    const WriteAck reply{ack.key};
    stats.complete();
    slot.reset();
    responsePort(reply.key.requester).sendWriteAck(reply);
}

} // namespace gem5::vector_engine
