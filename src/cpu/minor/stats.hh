/*
 * Copyright (c) 2011-2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

/**
 * @file
 *
 * The stats for MinorCPU separated from the CPU definition.
 */

#ifndef __CPU_MINOR_STATS_HH__
#define __CPU_MINOR_STATS_HH__

#include "base/statistics.hh"
#include "cpu/base.hh"
#include "sim/ticked_object.hh"

namespace gem5
{

namespace minor
{

/** Currently unused stats class. */
struct MinorStats : public statistics::Group
{
    /** vector_offload indica si la CPU usa la VPU. Sin ella, las métricas
     *  vectoriales se ocultan mientras valgan cero. */
    MinorStats(BaseCPU *parent, bool vector_offload = false);

    /** Number of cycles in quiescent state */
    statistics::Scalar quiesceCycles;

    /** Estadísticas relacionadas con la VPU en MinorCPU */
    /** Ciclos durante los cuales al menos una instrucción espera
     *  concesión (WaitGrant) */
    statistics::Scalar vectorWaitGrantCycles;

    /** Ciclos donde al menos una instrucción está en vuelo en la VPU
     *  esperando finalización (Dispatched o Accepted) */
    statistics::Scalar vectorWaitCompletionCycles;

    /** Ciclos donde una instrucción completada fuera del head
     *  está retenida esperando su turno de retiro en orden */
    statistics::Scalar vectorRetainedCompletionCycles;

    /** Número total de comandos vectoriales despachados hacia la VPU */
    statistics::Scalar vectorOffloadCommands;

    /** Estadísticas de concurrencia y ventana (Fase 2.5) */
    /** Ciclos con dos o más comandos simultáneamente activos en la VPU */
    statistics::Scalar vectorMultiActiveCycles;

    /** Ocupación de la ventana: ciclos según el número de comandos
     *  activos en la VPU (0, 1, 2, ...) */
    statistics::Vector vectorActiveCommandsDistribution;

    /** Bloqueos de la ventana concurrente. Motivo: límite de capacidad */
    statistics::Scalar vectorBlockedCapacity;

    /** Bloqueos de la ventana concurrente. Motivo: orden de programa no
     * consecutivo */
    statistics::Scalar vectorBlockedOrder;

    /** Bloqueos de la ventana concurrente. Motivo: operación no aritmética */
    statistics::Scalar vectorBlockedStructural;

    /** Bloqueos de la ventana concurrente. Motivo: instrucción aislada por
     * vstart!=0 */
    statistics::Scalar vectorBlockedVstart;

    /** Número total de instrucciones vectoriales rechazadas antes de emitirse
     *  a la VPU o rechazadas en admisión */
    statistics::Scalar vectorRejectedCommands;

    /** Comandos enteros despachados a la VPU
     */
    statistics::Scalar vectorAluCommands;

    /** Comandos de coma flotante despachados a la VPU
     */
    statistics::Scalar vectorFpuCommands;

    /** Comandos de memoria despachados a la VPU */
    statistics::Scalar vectorMemCommands;

    /** Comandos de slide despachados a la VPU */
    statistics::Scalar vectorSlideCommands;

    /** Comandos FP que produjeron excepciones IEEE 754 (fflags) */
    statistics::Scalar vectorFpExceptions;
};

} // namespace minor
} // namespace gem5

#endif /* __CPU_MINOR_STATS_HH__ */
