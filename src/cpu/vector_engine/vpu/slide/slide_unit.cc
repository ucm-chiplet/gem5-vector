/*
 * Copyright (c) 2026 Félix Garcia Narocki (UCM)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/vector_engine/vpu/slide/slide_unit.hh"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "cpu/vector_engine/common/element_codec.hh"
#include "cpu/vector_engine/common/operation_traits.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

bool
SlideUnit::LaneReceiver::reserveReception(TransferKey key, uint32_t bytes)
{
    return unit.reserveReception(lane, key, bytes);
}

void
SlideUnit::LaneReceiver::receive(const InterconnectPacket &packet)
{
    unit.receivePacket(lane, packet);
}

SlideUnit::SlideUnit(ClockedObject &owner, const AddressMapper &mapper,
                     const SlideParameters &parameters,
                     VectorInterconnect &network, ByteOrder target_byte_order,
                     VrfWakeup wake_vrf, CompletionSender send_completion)
    : stats(&owner, "slide", "slide tasks"),
      slide1UpTasks(&stats, "slide1UpTasks", statistics::units::Count::get(),
                    "Accepted vslide1up tasks"),
      slide1DownTasks(&stats, "slide1DownTasks",
                      statistics::units::Count::get(),
                      "Accepted vslide1down tasks"),
      sourceReads(&stats, "sourceReads", statistics::units::Count::get(),
                  "Source word reads granted by the VRF"),
      destinationWrites(&stats, "destinationWrites",
                        statistics::units::Count::get(),
                        "Destination word writes granted by the VRF"),
      localBytes(&stats, "localBytes", statistics::units::Byte::get(),
                 "Source bytes copied within their lane, without network"),
      remoteBytes(&stats, "remoteBytes", statistics::units::Byte::get(),
                  "Source bytes delivered by the interconnect"),
      packetsAccepted(&stats, "packetsAccepted",
                      statistics::units::Count::get(),
                      "Packets accepted by the interconnect"),
      packetRetries(&stats, "packetRetries", statistics::units::Count::get(),
                    "Packet attempts rejected by the interconnect"),
      scalarInsertions(&stats, "scalarInsertions",
                       statistics::units::Count::get(),
                       "Scalar elements written at an active index"),
      assemblyStalls(&stats, "assemblyStalls", statistics::units::Count::get(),
                     "Evaluations that found no assembly buffer for the "
                     "next destination word"),
      sourceBufferStalls(&stats, "sourceBufferStalls",
                         statistics::units::Count::get(),
                         "Evaluations that found no source buffer for the "
                         "next source word"),
      aliasWriteStalls(&stats, "aliasWriteStalls",
                       statistics::units::Count::get(),
                       "Complete destination words held until overlapping "
                       "source words were read (vd == vs2)"),
      owner(owner),
      mapper(mapper),
      parameters(parameters),
      network(network),
      targetByteOrder(target_byte_order),
      wakeVrf(std::move(wake_vrf)),
      sendCompletion(std::move(send_completion)),
      progressEvent([this] { evaluate(); }, owner.name() + ".slide", false,
                    VpuProgressPriority)
{
    fatal_if(!parameters.currentProfile(),
             "Slide unit requires positive VRF inputs and source buffers "
             "and at least two assembly buffers per lane");
    fatal_if(!wakeVrf || !sendCompletion,
             "Slide unit requires VRF and completion endpoints");
    const auto lanes = mapper.geometry().numLanes;
    fatal_if(network.capabilities().endpoints != lanes,
             "Slide unit requires one interconnect endpoint per lane");
    receivers.reserve(lanes);
    for (LaneId lane = 0; lane < lanes; ++lane) {
        receivers.push_back(std::make_unique<LaneReceiver>(*this, lane));
    }
}

SlideUnit::~SlideUnit()
{
    // Destruir la unidad no sustituye drain: no puede quedar una respuesta
    // del VRF ni una entrega de la red que la referencie.
    panic_if(!isIdle(), "Destroying slide unit before its work drained");
}

InterconnectReceiver &
SlideUnit::receiver(LaneId lane_id)
{
    panic_if(lane_id >= receivers.size(), "Invalid slide receiver lane");
    return *receivers[lane_id];
}

VrfRequestPort
SlideUnit::vrfPort(LaneId lane_id, uint32_t input)
{
    panic_if(lane_id >= mapper.geometry().numLanes ||
                 input >= parameters.vrfInputsPerLane,
             "Invalid slide VRF port lane or input");
    return {[this, lane_id, input] { return peekOffer(lane_id, input); },
            [this](const VrfAccessKey &key) { recvVrfGrant(key); }};
}

void
SlideUnit::validateTask(const SlideTask &task) const
{
    // El sequencer entrega una slide ya admitida. Una incoherencia aquí es
    // un error interno del protocolo, no una excepción del programa RVV.
    const auto &unit = task.task;
    const auto &config = task.config;
    const auto &slide = task.slide;
    panic_if(!unit.valid() || unit.unit != VectorUnitClass::Slide,
             "Invalid slide task identity or unit");
    panic_if(!VectorSupport::supportsConfiguration(config) ||
                 !VectorSupport::supportsSlide(slide, config.sewBits),
             "Unsupported configuration or operation in a slide task");
    // Una sola tarea cubre todo el rango activo: el escalar de down está en
    // vl-1 y up puede leer vs2[vstart-1], así que el plan necesita ambos.
    panic_if(unit.elements.firstElement != config.vstart ||
                 unit.elements.end() != config.vl,
             "Slide task must cover the whole active range");
    const auto element_bytes = VectorSupport::elementBytes(config.sewBits);
    const uint64_t offset =
        uint64_t{unit.elements.firstElement} * element_bytes;
    const uint64_t size = uint64_t{unit.elements.elementCount} * element_bytes;
    const auto capacity =
        VectorSupport::groupBytes(mapper.geometry().vlenBytes, config.lmul);
    panic_if(task.destinationRange.offset != offset ||
                 task.destinationRange.size != size ||
                 offset + size > capacity,
             "Slide byte range does not match its elements or group");
    const auto registers = VectorSupport::groupRegisterCount(config.lmul);
    for (const auto *reg : {&slide.destination, &slide.vectorSource}) {
        panic_if(!reg->naturallyAligned() || reg->regCount != registers,
                 "Slide register group does not match LMUL");
    }
    const auto *traits = operationTraits(slide.operation);
    panic_if(!traits->destinationMayOverlapSource &&
                 slide.destination == slide.vectorSource,
             "Admitted slide has a reserved source/destination overlap");
}

void
SlideUnit::addPieces(TaskState &state, std::size_t destination,
                     const ByteRange &range, int64_t shift) const
{
    // range son bytes de destino que proceden de vs2; shift convierte un
    // byte de destino en su byte de fuente (-SEW/8 en up, +SEW/8 en down).
    const auto max_packet = network.capabilities().maxPacketBytes;
    const auto &dest = state.destinations[destination];
    const int64_t first_source = int64_t{range.offset} + shift;
    panic_if(first_source < 0, "Slide source byte before the register group");
    auto position = static_cast<uint32_t>(first_source);
    const uint32_t end = position + range.size;
    while (position < end) {
        // Palabra fuente que contiene position: las fuentes están en orden
        // creciente de bytes y cubren el rango fuente sin huecos.
        const auto found = std::upper_bound(
            state.sources.begin(), state.sources.end(), position,
            [](uint32_t value, const SourceWord &word) {
                return value < word.access.range.offset;
            });
        panic_if(found == state.sources.begin(),
                 "Slide source byte outside the planned source words");
        const auto source =
            static_cast<std::size_t>(found - state.sources.begin()) - 1;
        auto &word = state.sources[source];
        panic_if(position >= word.access.range.end(),
                 "Slide source byte outside the planned source words");
        const uint32_t take =
            std::min(end, word.access.range.end()) - position;
        const bool remote = word.access.lane != dest.access.lane;
        // Una pieza remota se corta en paquetes del tamaño admitido por la
        // red; una local se copia de una vez en la misma lane.
        uint32_t covered = 0;
        while (covered < take) {
            uint32_t bytes = take - covered;
            if (remote) {
                bytes = std::min(bytes, max_packet);
            }
            const uint32_t source_offset = position + covered;
            const auto destination_offset =
                static_cast<uint32_t>(int64_t{source_offset} - shift);
            Piece piece;
            piece.source = source;
            piece.destination = destination;
            piece.sourceRange = ByteRange{source_offset, bytes};
            piece.destinationRange = ByteRange{destination_offset, bytes};
            piece.remote = remote;
            panic_if(state.pieces.size() >= InvalidTransferId,
                     "Slide task exhausted transfer identifiers");
            word.pieces.push_back(state.pieces.size());
            word.lastDestination = std::max(word.lastDestination, destination);
            state.pieces.push_back(piece);
            ++state.destinations[destination].missingPieces;
            covered += bytes;
        }
        position += take;
    }
}

void
SlideUnit::buildPlan(TaskState &state) const
{
    const auto &task = state.task;
    const auto &slide = task.slide;
    const bool up = slide.operation == SlideOperation::Slide1Up;
    const uint32_t eb = state.elementBytes;
    const uint32_t first = task.task.elements.firstElement;
    const uint32_t end = task.task.elements.end();

    // Palabras destino: el mapper es la única autoridad de ubicación.
    // Devuelve piezas de una palabra en orden creciente de bytes.
    const auto &dest_range = task.destinationRange;
    for (auto &fragment : mapper.map(slide.destination, dest_range,
                                     ByteEnable(dest_range.size, 1))) {
        DestinationWord word;
        word.access.lane = fragment.laneId;
        word.access.range = fragment.originalRange;
        state.destinations.push_back(std::move(word));
    }

    // El escalar sólo se escribe si su índice de destino está activo: 0 en
    // up (vstart == 0) y vl-1 en down (siempre, porque el rango no está
    // vacío). Las palabras contienen elementos enteros.
    std::optional<uint32_t> scalar_element;
    if (!up) {
        scalar_element = end - 1;
    } else if (first == 0) {
        scalar_element = 0;
    }
    if (scalar_element) {
        const uint32_t byte = *scalar_element * eb;
        for (std::size_t j = 0; j < state.destinations.size(); ++j) {
            const auto &range = state.destinations[j].access.range;
            if (byte >= range.offset && byte < range.end()) {
                panic_if(byte + eb > range.end(),
                         "Slide scalar element crosses a VRF word");
                state.scalarDestination = j;
                state.scalarOffset = byte - range.offset;
            }
        }
        panic_if(!state.scalarDestination,
                 "Slide scalar element has no destination word");
    }

    // Elementos fuente: up lee vs2[i-1] para i en [max(vstart, 1), vl) y
    // down lee vs2[i+1] para i en [vstart, vl-1). vl <= VLMAX, de modo que
    // ninguna lectura sale del grupo.
    uint32_t source_first = first + 1;
    uint32_t source_end = end;
    if (up) {
        source_first = std::max<uint32_t>(first, 1) - 1;
        source_end = end - 1;
    }
    if (source_end > source_first) {
        const ByteRange source_range{source_first * eb,
                                     (source_end - source_first) * eb};
        for (auto &fragment : mapper.map(slide.vectorSource, source_range,
                                         ByteEnable(source_range.size, 1))) {
            SourceWord word;
            word.access.lane = fragment.laneId;
            word.access.range = fragment.originalRange;
            state.sources.push_back(std::move(word));
        }
    }

    // Piezas: los bytes de cada palabra destino salvo el escalar proceden
    // de vs2 desplazados un elemento.
    int64_t shift = int64_t{eb};
    if (up) {
        shift = -int64_t{eb};
    }
    for (std::size_t j = 0; j < state.destinations.size(); ++j) {
        auto range = state.destinations[j].access.range;
        if (state.scalarDestination && *state.scalarDestination == j) {
            // El escalar está al principio del rango en up (índice 0) y al
            // final en down (índice vl-1).
            if (state.scalarOffset == 0) {
                range.offset += eb;
            } else {
                panic_if(state.scalarOffset + eb != range.size,
                         "Slide scalar is not at an end of its word");
            }
            range.size -= eb;
        }
        if (range.size) {
            addPieces(state, j, range, shift);
        }
    }
    for (const auto &word : state.sources) {
        panic_if(word.pieces.empty(), "Slide source word feeds no piece");
    }

    // vslide1down admite vd == vs2. Una escritura no puede destruir bytes
    // fuente que aún no se hayan leído: se anotan las fuentes que solapan
    // cada palabra destino. Con LMUL igual, los grupos coinciden o son
    // disjuntos, así que basta comparar el primer registro.
    // Destinos y fuentes están en orden creciente: basta un recorrido.
    if (slide.destination == slide.vectorSource) {
        std::size_t first_overlap = 0;
        for (auto &word : state.destinations) {
            const auto &range = word.access.range;
            while (first_overlap < state.sources.size() &&
                   state.sources[first_overlap].access.range.end() <=
                       range.offset) {
                ++first_overlap;
            }
            auto overlap_end = first_overlap;
            while (overlap_end < state.sources.size() &&
                   state.sources[overlap_end].access.range.offset <
                       range.end()) {
                ++overlap_end;
            }
            word.aliasFirst = first_overlap;
            word.aliasEnd = overlap_end;
        }
    }
}

TransferResult
SlideUnit::acceptTask(const SlideTask &task)
{
    // Retry no reserva recursos ni conserva referencias a la tarea recibida.
    if (active) {
        ++stats.retries;
        return TransferResult::Retry;
    }
    validateTask(task);
    active.emplace();
    auto &state = *active;
    state.task = task;
    state.elementBytes = VectorSupport::elementBytes(task.config.sewBits);
    const auto lanes = mapper.geometry().numLanes;
    state.assemblyInUse.assign(lanes, 0);
    state.sourcesInUse.assign(lanes, 0);
    state.offers.resize(lanes);
    buildPlan(state);
    if (task.slide.operation == SlideOperation::Slide1Up) {
        ++slide1UpTasks;
    } else {
        ++slide1DownTasks;
    }
    stats.accept(task.task.elements.elementCount, task.destinationRange.size);
    wakeup();
    return TransferResult::Accepted;
}

VrfAccess
SlideUnit::makeAccess(const TaskState &state, bool write,
                      std::size_t index) const
{
    // accessId 2k lee la fuente k y 2j+1 escribe el destino j.
    panic_if(index > (InvalidVrfAccessId - 2) / 2,
             "Slide task exhausted VRF access identifiers");
    auto access_id = static_cast<VrfAccessId>(2 * index);
    const SlideCommand &slide = state.task.slide;
    VectorRegRef reg = slide.vectorSource;
    ByteRange range;
    if (write) {
        ++access_id;
        reg = slide.destination;
        range = state.destinations[index].access.range;
    } else {
        range = state.sources[index].access.range;
    }
    return VrfAccess{
        VrfAccessKey{state.task.task.key,
                     VrfRequester{VrfRequesterKind::Slide, std::nullopt},
                     access_id},
        reg, range, ByteEnable(range.size, 1)};
}

SlideUnit::WordAccess &
SlideUnit::wordAccess(bool write, std::size_t index)
{
    if (write) {
        panic_if(index >= active->destinations.size(),
                 "Slide VRF event for an unknown destination word");
        return active->destinations[index].access;
    }
    panic_if(index >= active->sources.size(),
             "Slide VRF event for an unknown source word");
    return active->sources[index].access;
}

std::optional<VrfOffer>
SlideUnit::peekOffer(LaneId lane_id, uint32_t input) const
{
    if (!active || input >= active->offers[lane_id].size()) {
        return std::nullopt;
    }
    // La entrada input muestra el input-ésimo acceso ofrecido y aún no
    // concedido de esta lane; su petición no cambia hasta la concesión.
    const auto &offer = active->offers[lane_id][input];
    const WordAccess *access = &active->sources[offer.index].access;
    if (offer.write) {
        access = &active->destinations[offer.index].access;
    }
    panic_if(access->phase != AccessPhase::Offered || !access->request,
             "Slide offer has no stable request");
    return VrfOffer{&*access->request, access->eligibleAt,
                    access->publishedAt};
}

void
SlideUnit::recvVrfGrant(const VrfAccessKey &key)
{
    panic_if(!active || key.task != active->task.task.key ||
                 key.requester.kind != VrfRequesterKind::Slide,
             "Slide VRF grant with a foreign identity");
    const bool write = key.accessId % 2 == 1;
    const std::size_t index = key.accessId / 2;
    auto &access = wordAccess(write, index);
    panic_if(access.phase != AccessPhase::Offered ||
                 access.eligibleAt > curTick(),
             "Slide VRF grant in a wrong phase or before eligibility");
    // El LRF ya copió la petición; la oferta deja de ser visible.
    access.phase = AccessPhase::Granted;
    auto &offers = active->offers[access.lane];
    const auto found =
        std::find_if(offers.begin(), offers.end(), [&](const Offer &offer) {
            return offer.write == write && offer.index == index;
        });
    panic_if(found == offers.end(), "Granted slide access was not offered");
    offers.erase(found);
    if (write) {
        ++destinationWrites;
    } else {
        ++sourceReads;
    }
}

void
SlideUnit::publish(bool write, std::size_t index, ByteBuffer data,
                   std::vector<bool> &woken)
{
    auto &access = wordAccess(write, index);
    panic_if(access.phase != AccessPhase::Idle || access.request,
             "Slide word access published more than once");
    const auto vrf_access = makeAccess(*active, write, index);
    if (write) {
        access.request = VrfWriteRequest{vrf_access, std::move(data)};
    } else {
        access.request = VrfReadRequest{vrf_access};
    }
    // Elegible desde el siguiente flanco, como el resto de productores.
    access.eligibleAt = vpuClockEdge(owner, Cycles(1));
    access.publishedAt = curTick();
    access.phase = AccessPhase::Offered;
    active->offers[access.lane].push_back(Offer{write, index});
    woken[access.lane] = true;
}

bool
SlideUnit::allocateDestinations()
{
    // Reservar ensamblado en orden ascendente. La palabra más antigua sin
    // reserva retiene a las siguientes aunque otra lane tenga espacio.
    auto &state = *active;
    bool progress = false;
    while (state.nextAllocation < state.destinations.size()) {
        const auto index = state.nextAllocation;
        auto &word = state.destinations[index];
        auto &in_use = state.assemblyInUse[word.access.lane];
        if (in_use >= parameters.assemblyBuffersPerLane) {
            ++assemblyStalls;
            break;
        }
        ++in_use;
        word.allocated = true;
        word.data.assign(word.access.range.size, 0);
        if (state.scalarDestination && *state.scalarDestination == index) {
            // El escalar ya llega normalizado a SEW; se guarda con el orden
            // de bytes del objetivo, como cualquier elemento del VRF.
            ElementCodec::write(word.data,
                                state.scalarOffset / state.elementBytes,
                                state.task.config.sewBits,
                                state.task.slide.scalar, targetByteOrder);
            ++scalarInsertions;
        }
        ++state.nextAllocation;
        progress = true;
    }
    return progress;
}

bool
SlideUnit::publishReads(std::vector<bool> &woken)
{
    // Leer en orden ascendente sólo cuando todos los destinos de la palabra
    // tienen ensamblado: así sus bytes siempre tienen dónde quedarse.
    auto &state = *active;
    bool progress = false;
    while (state.nextRead < state.sources.size()) {
        const auto index = state.nextRead;
        auto &word = state.sources[index];
        if (word.lastDestination >= state.nextAllocation) {
            break;
        }
        auto &in_use = state.sourcesInUse[word.access.lane];
        if (in_use >= parameters.sourceBuffersPerLane) {
            ++sourceBufferStalls;
            break;
        }
        ++in_use;
        word.bufferHeld = true;
        publish(false, index, {}, woken);
        ++state.nextRead;
        progress = true;
    }
    return progress;
}

bool
SlideUnit::sendRemotePieces()
{
    // Cada pieza conserva su TransferKey y sus bytes hasta que la red la
    // acepta. Tras un Retry de una lane origen no se intentan más piezas de
    // esa lane en este ciclo; otras lanes pueden seguir enviando.
    auto &state = *active;
    bool progress = false;
    std::vector<bool> blocked(mapper.geometry().numLanes, false);
    auto it = state.readyRemote.begin();
    while (it != state.readyRemote.end()) {
        const auto index = *it;
        auto &piece = state.pieces[index];
        auto &source = state.sources[piece.source];
        const auto &destination = state.destinations[piece.destination];
        const auto lane = source.access.lane;
        if (blocked[lane]) {
            ++it;
            continue;
        }
        InterconnectPacket packet;
        packet.key =
            TransferKey{state.task.task.key, static_cast<TransferId>(index)};
        packet.sourceLane = lane;
        packet.destinationLane = destination.access.lane;
        packet.kind = TransferKind::RegisterMove;
        packet.sourceRange = piece.sourceRange;
        packet.destinationReg = state.task.slide.destination;
        packet.destinationRange = piece.destinationRange;
        const auto first = source.data.begin() + (piece.sourceRange.offset -
                                                  source.access.range.offset);
        packet.data.assign(first, first + piece.sourceRange.size);
        packet.byteEnable.assign(piece.sourceRange.size, 1);

        if (network.send(packet) == TransferResult::Retry) {
            ++packetRetries;
            blocked[lane] = true;
            ++it;
            continue;
        }
        // Una red podría entregar dentro de send(); no retroceder la fase.
        if (piece.phase == PiecePhase::Ready) {
            piece.phase = PiecePhase::InFlight;
        }
        ++packetsAccepted;
        --source.unsentPieces;
        releaseSourceIfDone(piece.source);
        it = state.readyRemote.erase(it);
        progress = true;
    }
    return progress;
}

bool
SlideUnit::aliasReady(const DestinationWord &word) const
{
    for (auto k = word.aliasFirst; k < word.aliasEnd; ++k) {
        if (active->sources[k].access.phase != AccessPhase::Done) {
            return false;
        }
    }
    return true;
}

bool
SlideUnit::publishWrites(std::vector<bool> &woken)
{
    // Una palabra completa se escribe en cuanto tiene todas sus piezas y,
    // con vd == vs2, cuando sus bytes fuente ya se han leído. Las palabras
    // pueden completarse en otro orden según las entregas de la red.
    auto &state = *active;
    bool progress = false;
    for (auto j = state.nextWriteScan; j < state.nextAllocation; ++j) {
        auto &word = state.destinations[j];
        if (word.access.phase != AccessPhase::Idle || word.missingPieces) {
            continue;
        }
        if (!aliasReady(word)) {
            ++aliasWriteStalls;
            continue;
        }
        publish(true, j, word.data, woken);
        progress = true;
    }
    while (state.nextWriteScan < state.nextAllocation &&
           state.destinations[state.nextWriteScan].access.phase !=
               AccessPhase::Idle) {
        ++state.nextWriteScan;
    }
    return progress;
}

void
SlideUnit::deliverPiece(std::size_t index, const ByteBuffer &bytes)
{
    auto &piece = active->pieces[index];
    auto &word = active->destinations[piece.destination];
    panic_if(!word.allocated || word.access.phase != AccessPhase::Idle ||
                 !word.missingPieces || piece.phase == PiecePhase::Delivered ||
                 bytes.size() != piece.destinationRange.size,
             "Slide piece delivered to a word that cannot receive it");
    // La posición procede de la pieza, no del orden de llegada.
    std::copy(bytes.begin(), bytes.end(),
              word.data.begin() +
                  (piece.destinationRange.offset - word.access.range.offset));
    --word.missingPieces;
    piece.phase = PiecePhase::Delivered;
    if (piece.remote) {
        remoteBytes += bytes.size();
    } else {
        localBytes += bytes.size();
    }
}

void
SlideUnit::releaseSourceIfDone(std::size_t index)
{
    // El buffer de fuente se libera cuando la lectura terminó y todas sus
    // piezas están copiadas o aceptadas por la red, que guarda su copia.
    auto &word = active->sources[index];
    if (!word.bufferHeld || word.access.phase != AccessPhase::Done ||
        word.unsentPieces) {
        return;
    }
    word.bufferHeld = false;
    --active->sourcesInUse[word.access.lane];
    word.data.clear();
    word.data.shrink_to_fit();
}

void
SlideUnit::recvVrfReadResponse(const ReadResponse &response)
{
    panic_if(!active || response.key.task != active->task.task.key ||
                 response.key.requester.kind != VrfRequesterKind::Slide ||
                 response.key.accessId % 2 != 0,
             "Unexpected slide VRF read response");
    const std::size_t index = response.key.accessId / 2;
    auto &word = active->sources[index];
    panic_if(word.access.phase != AccessPhase::Granted ||
                 response.data.size() != word.access.range.size,
             "Slide VRF read response has wrong phase or size");
    word.access.phase = AccessPhase::Done;
    word.access.request.reset();
    word.data = response.data;
    // Las piezas locales se copian ya en la misma lane; las remotas esperan
    // a que la red las acepte.
    for (const auto piece_index : word.pieces) {
        auto &piece = active->pieces[piece_index];
        const auto first = word.data.begin() + (piece.sourceRange.offset -
                                                word.access.range.offset);
        if (piece.remote) {
            piece.phase = PiecePhase::Ready;
            ++word.unsentPieces;
            active->readyRemote.push_back(piece_index);
        } else {
            deliverPiece(piece_index,
                         ByteBuffer(first, first + piece.sourceRange.size));
        }
    }
    releaseSourceIfDone(index);
    wakeup();
}

void
SlideUnit::recvVrfWriteAck(const WriteAck &ack)
{
    panic_if(!active || ack.key.task != active->task.task.key ||
                 ack.key.requester.kind != VrfRequesterKind::Slide ||
                 ack.key.accessId % 2 != 1,
             "Unexpected slide VRF write acknowledgement");
    const std::size_t index = ack.key.accessId / 2;
    auto &word = active->destinations[index];
    panic_if(word.access.phase != AccessPhase::Granted,
             "Slide VRF write acknowledgement in a wrong phase");
    // Sólo WriteAck libera el ensamblado y cuenta para cerrar la tarea.
    word.access.phase = AccessPhase::Done;
    word.access.request.reset();
    word.data.clear();
    word.data.shrink_to_fit();
    --active->assemblyInUse[word.access.lane];
    ++active->writesDone;
    wakeup();
}

bool
SlideUnit::reserveReception(LaneId lane, const TransferKey &key,
                            uint32_t bytes)
{
    panic_if(!active || key.task != active->task.task.key ||
                 key.transferId >= active->pieces.size(),
             "Slide reception for an unknown transfer");
    auto &piece = active->pieces[key.transferId];
    const auto &word = active->destinations[piece.destination];
    panic_if(!piece.remote || word.access.lane != lane ||
                 bytes != piece.destinationRange.size ||
                 piece.receptionReserved,
             "Slide reception does not match its planned piece");
    // El buffer de ensamblado ya guarda el sitio de estos bytes. Sin él no
    // se reserva nada y la red debe devolver Retry.
    if (!word.allocated) {
        return false;
    }
    piece.receptionReserved = true;
    return true;
}

void
SlideUnit::receivePacket(LaneId lane, const InterconnectPacket &packet)
{
    panic_if(!active || packet.key.task != active->task.task.key ||
                 packet.key.transferId >= active->pieces.size(),
             "Slide delivery for an unknown transfer");
    const auto &piece = active->pieces[packet.key.transferId];
    panic_if(!piece.receptionReserved || packet.destinationLane != lane ||
                 packet.destinationRange != piece.destinationRange ||
                 packet.sourceRange != piece.sourceRange ||
                 packet.destinationReg != active->task.slide.destination,
             "Slide delivery does not match its reserved piece");
    // Desde aquí la lane destino es dueña de los bytes; su escritura en el
    // VRF y el WriteAck siguen pendientes.
    deliverPiece(packet.key.transferId, packet.data);
    wakeup();
}

void
SlideUnit::wakeup()
{
    if (active && !progressEvent.scheduled()) {
        owner.schedule(progressEvent, vpuClockEdge(owner, Cycles(1)));
    }
}

void
SlideUnit::evaluate()
{
    if (!active) {
        return;
    }
    auto &state = *active;
    if (state.writesDone == state.destinations.size()) {
        finish();
        return;
    }
    // Un paso por evento: reservar ensamblado, leer fuentes, enviar piezas
    // remotas y publicar escrituras completas. Las respuestas del VRF y las
    // entregas de la red despiertan la siguiente evaluación.
    std::vector<bool> woken(mapper.geometry().numLanes, false);
    bool progress = allocateDestinations();
    progress = publishReads(woken) || progress;
    progress = sendRemotePieces() || progress;
    progress = publishWrites(woken) || progress;
    for (LaneId lane = 0; lane < woken.size(); ++lane) {
        if (woken[lane]) {
            wakeVrf(lane);
        }
    }
    // Volver a evaluar tras progreso (pueden haberse liberado recursos) o
    // si quedan piezas rechazadas por la red, que se reintentan.
    if (progress || !state.readyRemote.empty()) {
        wakeup();
    }
}

void
SlideUnit::finish()
{
    auto &state = *active;
    for (const auto &piece : state.pieces) {
        panic_if(piece.phase != PiecePhase::Delivered,
                 "Slide task finished with undelivered pieces");
    }
    for (const auto lanes : {&state.assemblyInUse, &state.sourcesInUse}) {
        panic_if(std::any_of(lanes->begin(), lanes->end(),
                             [](uint32_t count) { return count != 0; }),
                 "Slide task finished with held buffers");
    }
    const UnitCompletion completion{
        state.task.task.key, UnitCompletionStatus::Success, std::nullopt};
    // Liberar la capacidad antes del callback permite admitir otra tarea.
    stats.complete();
    active.reset();
    sendCompletion(completion);
}

} // namespace gem5::vector_engine
