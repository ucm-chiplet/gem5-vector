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
    supported_sews = VectorParam.UInt32(
        [16, 32], "Active backend SEWs in bits"
    )
    elen_bits = Param.UInt32(64, "Must match the connected CPU ELEN")
    max_active_commands = Param.UInt32(1, "Phase 1 requires exactly one")
    mapping_policy = Param.String("word_striped", "VRF placement policy")
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
    max_bundle_bytes = Param.UInt32(0, "Zero resolves to lane_word_bytes")
    alus_per_lane = Param.UInt32(1, "Phase 1 requires exactly one")
    alu_width_bytes = Param.UInt32(0, "Zero resolves to lane_word_bytes")
    alu_capacity = Param.UInt32(1, "Pending bundles per ALU")
    alu_latency = Param.Cycles(1, "Functional ALU response scheduling")
    alu_initiation_interval = Param.Cycles(1, "Prepared service contract")
    fpu_width_bytes = Param.UInt32(0, "Zero resolves to lane_word_bytes")
    fpu_capacity = Param.UInt32(1, "Pending bundles per FPU")
    fpu_latency = Param.Cycles(1, "Functional FPU response scheduling")
    fpu_initiation_interval = Param.Cycles(1, "Prepared service contract")
    bank_read_ports = Param.UInt32(0, "Dedicated read ports per bank")
    bank_write_ports = Param.UInt32(0, "Dedicated write ports per bank")
    bank_read_write_ports = Param.UInt32(1, "Shared RW ports per bank")
    bank_access_entries = Param.UInt32(1, "Pending accesses per bank")
    bank_latency = Param.Cycles(1, "Functional bank response scheduling")
    bank_initiation_interval = Param.Cycles(1, "Prepared service contract")
    memory_max_block_bytes = Param.UInt32(8, "Prepared block byte limit")
    memory_active_blocks = Param.UInt32(1, "Phase 1 requires exactly one")
    memory_block_requests = Param.Bool(False, "Reserved for phase 4")
    memory_max_outstanding_packets = Param.UInt32(1, "Physical packet limit")
    interconnect_enabled = Param.Bool(False, "No active network in phase 1")
    interconnect_max_packet_bytes = Param.UInt32(0, "Zero resolves to a word")
    interconnect_packets_per_destination = Param.UInt32(1, "Packet entries")
    interconnect_bytes_per_destination = Param.UInt32(
        0, "Receive buffer bytes"
    )
    interconnect_source_bytes_per_cycle = Param.UInt32(
        0, "Injection bytes/cycle"
    )
    interconnect_destination_bytes_per_cycle = Param.UInt32(
        0, "Delivery bytes/cycle"
    )
    interconnect_transit_latency = Param.Cycles(1, "Transit excluding service")
    address_bits = Param.Unsigned("Target address width: 32 or 64")
    target_byte_order = Param.ByteOrder("Target CPU byte order")

    mem_side = RequestPort("Vector memory timing requests and responses")

    @classmethod
    def from_configuration(cls, configuration, **kwargs):
        """Construye desde una composición tipada y argumentos gem5 adicionales.

        Rechaza ajustes duplicados para que cada valor tenga un único origen.
        La validación de geometría y recursos pertenece al constructor C++;
        este adaptador no conecta puertos ni inicia trabajo.
        """
        from m5.objects.configuration import VpuConfiguration

        if not isinstance(configuration, VpuConfiguration):
            raise TypeError("Expected a VpuConfiguration")
        values = configuration.parameters()
        duplicates = values.keys() & kwargs.keys()
        if duplicates:
            raise ValueError(f"Duplicate VPU settings: {sorted(duplicates)}")
        return cls(**values, **kwargs)
