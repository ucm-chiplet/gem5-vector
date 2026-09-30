# Copyright (c) 2026 Félix Garcia Narocki (UCM)
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.ClockedObject import ClockedObject
from m5.params import (
    Param,
    RequestPort,
    VectorParam,
)


class VectorEngine(ClockedObject):
    type = "VectorEngine"
    cxx_class = "gem5::vector_engine::VectorEngine"
    cxx_header = "cpu/vector_engine/vector_engine.hh"

    # La configuración de ejecución fija geometría y soporte explícitamente.
    vlen_bytes = Param.UInt32("Vector register length in bytes")
    lane_word_bytes = Param.UInt32("Bytes per lane word; multiple of four")
    num_lanes = Param.UInt32("Number of physical lanes")
    banks_per_lane = Param.UInt32("Number of VRF banks per lane")
    supported_lmuls = VectorParam.Int(
        "Supported LMUL exponents: -3 (mf8) through 3 (m8)"
    )
    command_queue_entries = Param.UInt32(
        1, "Command capacity including reservations and the active head"
    )
    lane_task_entries = Param.UInt32(1, "Baseline requires exactly one")
    operand_buffer_entries_per_source = Param.UInt32(
        1, "Buffers per vector source and lane; baseline requires one"
    )
    result_buffer_entries = Param.UInt32(
        1, "Result buffers per lane; baseline requires one"
    )
    address_bits = Param.Unsigned("Target address width: 32 or 64")
    target_byte_order = Param.ByteOrder("Target CPU byte order")

    mem_side = RequestPort("Vector memory timing requests and responses")
