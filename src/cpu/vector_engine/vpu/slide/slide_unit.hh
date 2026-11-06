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

#ifndef __CPU_VECTOR_ENGINE_VPU_SLIDE_SLIDE_UNIT_HH__
#define __CPU_VECTOR_ENGINE_VPU_SLIDE_SLIDE_UNIT_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cpu/vector_engine/common/backend_task.hh"
#include "cpu/vector_engine/common/module_stats.hh"
#include "cpu/vector_engine/common/unit_completion.hh"
#include "cpu/vector_engine/vpu/interconnect/vector_interconnect.hh"
#include "cpu/vector_engine/vpu/register_file/address_mapper.hh"
#include "cpu/vector_engine/vpu/register_file/vrf_port.hh"
#include "enums/ByteOrder.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::vector_engine
{

/**
 * Ejecutor de vslide1up.vx y vslide1down.vx sin predicación (RVV 1.0,
 * edición 20240411, Vector Slide Instructions). Es una unidad global, como
 * la SLDU de Ara, con un agente por lane: cada agente ofrece accesos a su
 * LRF y recibe los paquetes de la red dirigidos a esa lane. No es una SLDU
 * completa: sólo desplaza un elemento.
 *
 * Plan de la tarea. El rango activo de destino es [vstart, vl). El mapper
 * divide sus bytes en palabras destino, cada una en su lane. Las fuentes se
 * derivan del desplazamiento, no del rango de destino: up lee vs2[i-1] y
 * down vs2[i+1]; el escalar ocupa el índice 0 (up, sólo si vstart == 0) o
 * vl-1 (down). Las palabras fuente se obtienen también del mapper. Cada
 * palabra destino se forma con piezas: bytes de una palabra fuente que van
 * a ella. Una pieza es local si fuente y destino están en la misma lane;
 * si no, viaja por VectorInterconnect en paquetes de como máximo
 * maxPacketBytes, con TransferKey = (TaskKey, índice de pieza).
 *
 * Recursos finitos por lane y su ciclo de vida:
 *
 * - Buffer de ensamblado: se reserva para una palabra destino en orden
 *   ascendente, antes de leer sus fuentes. Incluye la recepción de sus
 *   piezas remotas, así que reserveReception() siempre puede aceptar. Se
 *   libera con el WriteAck de la palabra.
 * - Buffer de fuente: se reserva al publicar la lectura de una palabra
 *   fuente, sólo cuando todos sus destinos tienen ensamblado. Se libera
 *   cuando sus piezas locales se han copiado y las remotas se han aceptado
 *   en la red. La red copia los datos aceptados.
 *
 * Las lecturas se publican en orden ascendente de palabra. Como cada fuente
 * alimenta como mucho dos destinos consecutivos y hay al menos dos buffers
 * de ensamblado por lane, la palabra destino más antigua siempre progresa.
 *
 * Solapamiento. Admisión rechaza vd == vs2 en up, porque RVV lo reserva. En
 * down lo admite: la escritura de una palabra destino espera a que se hayan
 * leído todas las palabras fuente que solapan sus bytes. No depende del
 * orden de llegada de los paquetes ni del scoreboard, que no ve riesgos
 * internos de un comando.
 *
 * Las escrituras sólo habilitan los bytes de [vstart, vl): los elementos
 * anteriores y el tail se conservan, lo que también es válido con vta=1.
 * La tarea termina con el último WriteAck; recibir un paquete no completa
 * su escritura. Una sola tarea activa a la vez; otra recibe Retry.
 *
 * Identidades: VrfAccessKey usa requester Slide y accessId 2k para leer la
 * fuente k y 2j+1 para escribir el destino j, únicos en la tarea. Las
 * respuestas y entregas se localizan por clave, nunca por orden.
 *
 * Propietario, mapper, red, LRF y receptores sobreviven al módulo hasta
 * isIdle(). Los callbacks no pueden destruirlo. Tras un Retry de la red, el
 * paquete y su clave se conservan y se reintentan en el ciclo siguiente.
 */
class SlideUnit
{
  public:
    using VrfWakeup = std::function<void(LaneId)>;
    using CompletionSender = std::function<void(const UnitCompletion &)>;

    SlideUnit(ClockedObject &owner, const AddressMapper &mapper,
              const SlideParameters &parameters, VectorInterconnect &network,
              ByteOrder target_byte_order, VrfWakeup wake_vrf,
              CompletionSender send_completion);
    ~SlideUnit();
    SlideUnit(const SlideUnit &) = delete;
    SlideUnit &operator=(const SlideUnit &) = delete;

    // Copia la tarea si no hay otra activa; Accepted no indica finalización.
    TransferResult acceptTask(const SlideTask &task);
    // Entrada persistente input del LRF lane_id: ofrece el input-ésimo
    // acceso publicado y no concedido de esa lane.
    VrfRequestPort vrfPort(LaneId lane_id, uint32_t input);
    // Receptor de la red para lane_id; vive tanto como esta unidad.
    InterconnectReceiver &receiver(LaneId lane_id);
    void recvVrfReadResponse(const ReadResponse &response);
    void recvVrfWriteAck(const WriteAck &ack);
    // Programa evaluación en el siguiente ciclo, nunca avanza directamente.
    void wakeup();

    // Estado local; el propietario consulta también red y LRF.
    bool
    isIdle() const
    {
        return !active && !progressEvent.scheduled();
    }

  private:
    /** Agente receptor de una lane; reenvía a la unidad con su lane. */
    class LaneReceiver : public InterconnectReceiver
    {
      public:
        LaneReceiver(SlideUnit &unit, LaneId lane) : unit(unit), lane(lane) {}
        bool reserveReception(TransferKey key, uint32_t bytes) override;
        void receive(const InterconnectPacket &packet) override;

      private:
        SlideUnit &unit;
        const LaneId lane;
    };

    enum class AccessPhase
    {
        Idle,
        Offered,
        Granted,
        Done
    };
    enum class PiecePhase
    {
        // Esperando los datos de su fuente.
        Waiting,
        // Datos capturados; una pieza remota espera que la red la acepte.
        Ready,
        // Aceptada por la red, todavía sin entregar.
        InFlight,
        // Bytes copiados en el buffer de ensamblado.
        Delivered
    };

    /** Acceso al VRF de una palabra, publicado hasta su respuesta. */
    struct WordAccess
    {
        LaneId lane = 0;
        // Bytes relativos al grupo; están en una sola palabra del VRF.
        ByteRange range;
        AccessPhase phase = AccessPhase::Idle;
        std::optional<VrfRequest> request;
        Tick eligibleAt = 0;
        Tick publishedAt = 0;
    };
    struct SourceWord
    {
        WordAccess access;
        // Piezas que se cortan de esta palabra y destino más joven que
        // alimentan; la lectura espera a que éste tenga ensamblado.
        std::vector<std::size_t> pieces;
        std::size_t lastDestination = 0;
        ByteBuffer data;
        // Piezas remotas capturadas que la red aún no ha aceptado.
        std::size_t unsentPieces = 0;
        bool bufferHeld = false;
    };
    struct DestinationWord
    {
        WordAccess access;
        bool allocated = false;
        ByteBuffer data;
        std::size_t missingPieces = 0;
        // Fuentes [aliasFirst, aliasEnd) que solapan estos bytes cuando
        // vd == vs2; deben estar leídas antes de publicar la escritura.
        std::size_t aliasFirst = 0;
        std::size_t aliasEnd = 0;
    };
    struct Piece
    {
        std::size_t source = 0;
        std::size_t destination = 0;
        ByteRange sourceRange;
        ByteRange destinationRange;
        bool remote = false;
        bool receptionReserved = false;
        PiecePhase phase = PiecePhase::Waiting;
    };
    /** Acceso ofrecido a un LRF: lectura de fuente o escritura de destino. */
    struct Offer
    {
        bool write = false;
        std::size_t index = 0;
    };
    struct TaskState
    {
        SlideTask task;
        uint32_t elementBytes = 0;
        std::vector<SourceWord> sources;
        std::vector<DestinationWord> destinations;
        std::vector<Piece> pieces;
        // Palabra destino que recibe el escalar, si su índice está activo.
        std::optional<std::size_t> scalarDestination;
        uint32_t scalarOffset = 0;
        std::size_t nextAllocation = 0;
        std::size_t nextRead = 0;
        std::size_t nextWriteScan = 0;
        std::size_t writesDone = 0;
        std::vector<uint32_t> assemblyInUse;
        std::vector<uint32_t> sourcesInUse;
        // Accesos ofrecidos y no concedidos, por lane y en orden.
        std::vector<std::deque<Offer>> offers;
        // Piezas remotas listas para enviar, en orden de captura.
        std::deque<std::size_t> readyRemote;
    };

    ModuleStats stats;
    statistics::Scalar slide1UpTasks;
    statistics::Scalar slide1DownTasks;
    statistics::Scalar sourceReads;
    statistics::Scalar destinationWrites;
    statistics::Scalar localBytes;
    statistics::Scalar remoteBytes;
    statistics::Scalar packetsAccepted;
    statistics::Scalar packetRetries;
    statistics::Scalar scalarInsertions;
    statistics::Scalar assemblyStalls;
    statistics::Scalar sourceBufferStalls;
    statistics::Scalar aliasWriteStalls;
    ClockedObject &owner;
    const AddressMapper &mapper;
    const SlideParameters &parameters;
    VectorInterconnect &network;
    const ByteOrder targetByteOrder;
    VrfWakeup wakeVrf;
    CompletionSender sendCompletion;
    std::vector<std::unique_ptr<LaneReceiver>> receivers;
    std::optional<TaskState> active;
    EventFunctionWrapper progressEvent;

    void validateTask(const SlideTask &task) const;
    void buildPlan(TaskState &state) const;
    void addPieces(TaskState &state, std::size_t destination,
                   const ByteRange &range, int64_t shift) const;
    VrfAccess makeAccess(const TaskState &state, bool write,
                         std::size_t index) const;
    WordAccess &wordAccess(bool write, std::size_t index);
    std::optional<VrfOffer> peekOffer(LaneId lane_id, uint32_t input) const;
    void recvVrfGrant(const VrfAccessKey &key);
    void publish(bool write, std::size_t index, ByteBuffer data,
                 std::vector<bool> &woken);
    bool allocateDestinations();
    bool publishReads(std::vector<bool> &woken);
    bool sendRemotePieces();
    bool publishWrites(std::vector<bool> &woken);
    bool aliasReady(const DestinationWord &word) const;
    void deliverPiece(std::size_t piece, const ByteBuffer &bytes);
    void releaseSourceIfDone(std::size_t source);
    bool reserveReception(LaneId lane, const TransferKey &key, uint32_t bytes);
    void receivePacket(LaneId lane, const InterconnectPacket &packet);
    void evaluate();
    void finish();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_SLIDE_SLIDE_UNIT_HH__
