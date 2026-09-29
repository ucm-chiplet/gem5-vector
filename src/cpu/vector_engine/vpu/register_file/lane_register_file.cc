// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/register_file/lane_register_file.hh"

#include <algorithm>
#include <utility>

#include "base/logging.hh"

namespace gem5::vector_engine
{

LaneRegisterFile::LaneRegisterFile(ClockedObject &owner,
                                   const AddressMapper &mapper, LaneId lane_id,
                                   ResponsePort lane_responses,
                                   ResponsePort vlsu_responses)
    : mapper(mapper),
      laneId(lane_id),
      laneResponses(std::move(lane_responses)),
      vlsuResponses(std::move(vlsu_responses))
{
    const auto &geometry = mapper.geometry();
    fatal_if(laneId >= geometry.numLanes,
             "LRF identity is outside the VRF geometry");
    fatal_if(!laneResponses.sendRead || !laneResponses.sendWriteAck ||
                 !vlsuResponses.sendRead || !vlsuResponses.sendWriteAck,
             "LRF requires read and write acknowledgement endpoints");
    fatal_if(geometry.banksPerLane > banks.max_size() ||
                 geometry.banksPerLane > pending.max_size(),
             "LRF bank count exceeds the host container capacity");
    banks.reserve(geometry.banksPerLane);
    pending.resize(geometry.banksPerLane);
    for (BankId bank_id = 0; bank_id < geometry.banksPerLane; ++bank_id) {
        banks.push_back(std::make_unique<VectorRegBank>(
            owner, mapper, laneId, bank_id,
            [this, bank_id](const BankReadResponse &response) {
                recvBankReadResponse(bank_id, response);
            },
            [this, bank_id](const BankWriteAck &ack) {
                recvBankWriteAck(bank_id, ack);
            }));
    }
}

LaneRegisterFile::~LaneRegisterFile()
{
    panic_if(!isIdle(), "Destroying LRF before its work has drained");
}

bool
LaneRegisterFile::isIdle() const
{
    return std::none_of(pending.begin(), pending.end(),
                        [](const auto &entry) { return bool(entry); }) &&
           std::all_of(banks.begin(), banks.end(),
                       [](const auto &bank) { return bank->isIdle(); });
}

MappedVrfFragment
LaneRegisterFile::validateAccess(const VrfAccess &access) const
{
    panic_if(!access.key.valid() ||
                 (access.key.requester.kind == VrfRequesterKind::Lane &&
                  *access.key.requester.laneId != laneId),
             "Invalid requester or identity in an LRF access");
    // Rechazar antes del mapeo rangos mayores que una palabra evita crear
    // fragmentos que este receptor no puede aceptar como una sola operación.
    panic_if(!access.range.valid() ||
                 access.range.size > mapper.geometry().laneWordBytes,
             "LRF access must fit in one word");
    auto mapping = mapper.map(access.reg, access.range, access.byteEnable);
    panic_if(mapping.size() != 1 || mapping.front().laneId != laneId ||
                 mapping.front().originalRange != access.range,
             "LRF access does not belong to one local VRF word");
    // Las claves en vuelo son únicas incluso si apuntan a bancos distintos.
    panic_if(std::any_of(pending.begin(), pending.end(),
                         [&](const auto &entry) {
                             return entry && entry->access.key == access.key;
                         }),
             "Duplicate accepted LRF access");
    return std::move(mapping.front());
}

TransferResult
LaneRegisterFile::read(const VrfReadRequest &request)
{
    const auto &access = request.access;
    auto mapping = validateAccess(access);
    const auto bank_id = mapping.bankId;
    if (pending[bank_id]) {
        return TransferResult::Retry;
    }
    // El descriptor y el buffer de retorno existen antes de llamar al banco.
    // La petición de banco es local: una respuesta puede retirar pending.
    const BankReadRequest bank_request{access.key, mapping.row,
                                       mapping.wordByteEnable};
    pending[bank_id] =
        PendingAccess{access, std::move(mapping), AccessKind::Read,
                      ByteBuffer(access.range.size, 0)};
    const auto result = banks[bank_id]->read(bank_request);
    handleTransfer(bank_id, result);
    return result;
}

TransferResult
LaneRegisterFile::write(const VrfWriteRequest &request)
{
    const auto &access = request.access;
    auto mapping = validateAccess(access);
    panic_if(request.data.size() != access.range.size,
             "LRF write data does not match its byte range");
    const auto bank_id = mapping.bankId;
    if (pending[bank_id]) {
        return TransferResult::Retry;
    }
    // Los bytes exteriores al rango quedan a cero y tienen máscara cero.
    ByteBuffer word_data(mapper.geometry().laneWordBytes, 0);
    std::copy(request.data.begin(), request.data.end(),
              word_data.begin() + mapping.byteOffsetInWord);
    const BankWriteRequest bank_request{
        access.key, mapping.row, mapping.wordByteEnable, std::move(word_data)};
    pending[bank_id] =
        PendingAccess{access, std::move(mapping), AccessKind::Write, {}};
    const auto result = banks[bank_id]->write(bank_request);
    handleTransfer(bank_id, result);
    return result;
}

void
LaneRegisterFile::handleTransfer(BankId bank_id, TransferResult result)
{
    switch (result) {
        case TransferResult::Accepted:
            // No recrear una entrada retirada por una respuesta síncrona.
            break;
        case TransferResult::Retry:
            panic_if(!pending[bank_id],
                     "VRF bank responded to an access it rejected");
            pending[bank_id].reset();
            break;
        default:
            panic("Invalid VRF bank transfer result");
    }
}

const LaneRegisterFile::ResponsePort &
LaneRegisterFile::responsePort(const VrfRequester &requester) const
{
    panic_if(!requester.valid() || (requester.kind == VrfRequesterKind::Lane &&
                                    *requester.laneId != laneId),
             "Invalid requester in an LRF response");
    return requester.kind == VrfRequesterKind::Lane ? laneResponses
                                                    : vlsuResponses;
}

void
LaneRegisterFile::recvBankReadResponse(BankId bank_id,
                                       const BankReadResponse &response)
{
    panic_if(bank_id >= pending.size() || !pending[bank_id],
             "VRF bank read response has no pending LRF access");
    auto &slot = pending[bank_id];
    panic_if(slot->kind != AccessKind::Read ||
                 slot->access.key != response.key ||
                 slot->mapping.bankId != bank_id ||
                 response.wordData.size() != mapper.geometry().laneWordBytes,
             "VRF bank read response has wrong identity, direction or size");
    ReadResponse reply{response.key, std::move(slot->readData)};
    std::copy_n(response.wordData.begin() + slot->mapping.byteOffsetInWord,
                reply.data.size(), reply.data.begin());
    // Se libera antes de notificar: el solicitante puede emitir otro acceso.
    slot.reset();
    responsePort(reply.key.requester).sendRead(reply);
}

void
LaneRegisterFile::recvBankWriteAck(BankId bank_id, const BankWriteAck &ack)
{
    panic_if(bank_id >= pending.size() || !pending[bank_id],
             "VRF bank write acknowledgement has no pending LRF access");
    auto &slot = pending[bank_id];
    panic_if(slot->kind != AccessKind::Write || slot->access.key != ack.key ||
                 slot->mapping.bankId != bank_id,
             "VRF bank write acknowledgement has wrong identity or direction");
    const WriteAck reply{ack.key};
    slot.reset();
    responsePort(reply.key.requester).sendWriteAck(reply);
}

} // namespace gem5::vector_engine
