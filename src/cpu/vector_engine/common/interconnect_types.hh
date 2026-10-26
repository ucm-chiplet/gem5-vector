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

#ifndef __CPU_VECTOR_ENGINE_COMMON_INTERCONNECT_TYPES_HH__
#define __CPU_VECTOR_ENGINE_COMMON_INTERCONNECT_TYPES_HH__

#include <cstdint>
#include <limits>

#include "cpu/vector_engine/common/vrf_types.hh"

namespace gem5::vector_engine
{

using TransferId = uint32_t;
inline constexpr TransferId InvalidTransferId =
    std::numeric_limits<TransferId>::max();

/** Identifica un envío dentro de su tarea, aunque haya varios emisores. */
struct TransferKey
{
    TaskKey task;
    TransferId transferId = InvalidTransferId;

    /** Indica si tarea y número de envío tienen valores válidos. */
    bool
    valid() const
    {
        return task.valid() && transferId != InvalidTransferId;
    }
};

/** Compara también la tarea y el comando; transferId aislado no es único. */
inline bool
operator==(const TransferKey &lhs, const TransferKey &rhs)
{
    return lhs.task == rhs.task && lhs.transferId == rhs.transferId;
}

/** Desigualdad de la clave completa, con el mismo ámbito que operator==. */
inline bool
operator!=(const TransferKey &lhs, const TransferKey &rhs)
{
    return !(lhs == rhs);
}

/** Indica para qué se mueve el dato; no contiene una instrucción CPU. */
enum class TransferKind : uint8_t
{
    RegisterMove
};

/**
 * Límites que anuncia la red: cuántos destinos conecta, cuánto cabe en un
 * paquete y cuánto puede guardar o mover. Los tamaños se expresan en bytes
 * y las velocidades en bytes por ciclo. Declararlos no construye una red.
 */
struct InterconnectCapabilities
{
    uint32_t endpoints = 0;
    uint32_t maxPacketBytes = 0;
    uint32_t packetsPerDestination = 0;
    uint32_t bytesPerDestination = 0;
    uint32_t sourceBytesPerCycle = 0;
    uint32_t destinationBytesPerCycle = 0;
};

/**
 * Datos de un envío entre lanes. El paquete incluye su propia copia de los
 * bytes. Si la red lo acepta, debe conservarlos hasta entregarlos una vez.
 * Si pide reintentar, el emisor sigue siendo responsable del envío. Quien
 * recibe los bytes debe encargarse de su escritura y confirmación en el VRF.
 */
struct InterconnectPacket
{
    TransferKey key;
    LaneId sourceLane = 0;
    LaneId destinationLane = 0;
    TransferKind kind = TransferKind::RegisterMove;
    ByteRange sourceRange;
    VectorRegRef destinationReg;
    ByteRange destinationRange;
    ByteBuffer data;
    ByteEnable byteEnable;

    /**
     * Comprueba destinos, tamaños y bytes habilitados frente a los límites
     * anunciados. No reserva espacio ni comprueba si los datos caben en el
     * VRF o si la instrucción cumple las reglas de RVV.
     */
    bool valid(const InterconnectCapabilities &caps) const;
};

/**
 * Momentos que puede registrar cualquier red: intento, aceptación y entrega.
 * La espera va del primer intento a la aceptación; el viaje observado, de
 * la aceptación a la entrega. Reintentar no cuenta como otro paquete ni
 * como más datos transferidos.
 */
enum class TransportObservation : uint8_t
{
    Attempt,
    Accepted,
    Delivered
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_INTERCONNECT_TYPES_HH__
