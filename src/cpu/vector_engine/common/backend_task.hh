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

#ifndef __CPU_VECTOR_ENGINE_COMMON_BACKEND_TASK_HH__
#define __CPU_VECTOR_ENGINE_COMMON_BACKEND_TASK_HH__

#include "cpu/vector_engine/common/unit_task.hh"
#include "cpu/vector_engine/common/vector_reg_ref.hh"
#include "cpu/vector_engine/interface/vector_command.hh"

namespace gem5::vector_engine
{

/**
 * AraSequencer la crea para que TaskDistributor reparta el trabajo aritmético.
 * Conserva el comando ya validado; task.unit debe ser Lanes.
 */
struct ArithmeticTask
{
    UnitTask task;
    VectorConfig config;
    ArithmeticCommand arithmetic;
    ByteRange destinationRange;
};

/**
 * AraSequencer la crea para que AraVLSU ejecute la carga o el store.
 * Conserva los metadatos de memoria del comando; task.unit debe ser Vlsu.
 */
struct MemoryTask
{
    UnitTask task;
    VectorConfig config;
    MemoryCommand memory;

    // AraVLSU usa este rango como destino de carga o fuente de store.
    // El desplazamiento es relativo al principio del grupo de registros.
    ByteRange dataRange;
    Addr pc = 0;
    RequestorID requestorId = Request::invldRequestorId;
};

// AraSequencer crea una tarea para [vstart, vl) en la versión inicial.
// Para elementos de 32 bits, el rango comienza en firstElement * 4 y ocupa
// elementCount * 4 bytes. Si vstart >= vl, no crea ninguna tarea.

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_BACKEND_TASK_HH__
