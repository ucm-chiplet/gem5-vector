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
        [16, 32], "Enabled backend SEWs; implemented widths: 8/16/32/64"
    )
    elen_bits = Param.UInt32(64, "Must match the connected CPU ELEN")
    max_active_commands = Param.UInt32(
        4, "Commands started and not yet completed; <= command_queue_entries"
    )
    mapping_policy = Param.String("word_striped", "VRF placement policy")
    command_queue_entries = Param.UInt32(
        4, "Command capacity: reservations, pending and active commands"
    )
    lane_task_entries = Param.UInt32(
        4, "Fragment contexts per lane until writeback, shared by tasks"
    )
    max_bundle_bytes = Param.UInt32(0, "Zero resolves to lane_word_bytes")
    alus_per_lane = Param.UInt32(1, "Independent ALU instances per lane")
    # Grupo de ALUs: buffers propios de la clase y perfil por operación.
    alu_width_bytes = Param.UInt32(0, "Zero resolves to lane_word_bytes")
    alu_capacity = Param.UInt32(1, "Pending bundles per ALU instance")
    alu_operand_entries_per_source = Param.UInt32(
        1, "ALU-class buffers per source and lane, including scalars"
    )
    alu_result_entries = Param.UInt32(
        1, "ALU-class result credits per lane, until WriteAck"
    )
    alu_add_latency = Param.Cycles(1, "Integer add latency in VPU cycles")
    alu_add_initiation_interval = Param.Cycles(
        1, "Minimum VPU cycles between integer add admissions"
    )
    alu_multiply_latency = Param.Cycles(
        3, "Integer multiply latency in VPU cycles"
    )
    alu_multiply_initiation_interval = Param.Cycles(
        1, "Minimum VPU cycles between integer multiply admissions"
    )
    # vmin/vminu: comparador segmentado. El valor por defecto replica la
    # suma; es un parámetro del modelo, no una latencia calibrada.
    alu_minimum_latency = Param.Cycles(
        1, "Integer minimum latency in VPU cycles"
    )
    alu_minimum_initiation_interval = Param.Cycles(
        1, "Minimum VPU cycles between integer minimum admissions"
    )
    alu_divide_latencies = VectorParam.UInt32(
        [10, 18, 34, 66],
        "Unpipelined divide/remainder latency for SEW 8/16/32/64",
    )
    # FPU: pipeline de suma/multiplicación y divisor no segmentado.
    fpu_width_bytes = Param.UInt32(0, "Zero resolves to lane_word_bytes")
    fpu_capacity = Param.UInt32(
        1, "Pending bundles in the FPU add/multiply pipeline"
    )
    fpu_operand_entries_per_source = Param.UInt32(
        1, "FPU-class buffers per source and lane"
    )
    fpu_result_entries = Param.UInt32(
        1, "FPU-class result credits per lane, until WriteAck"
    )
    fpu_add_multiply_latency = Param.Cycles(
        4, "FP add/multiply latency in VPU cycles"
    )
    fpu_add_multiply_initiation_interval = Param.Cycles(
        1, "Minimum VPU cycles between FP add/multiply admissions"
    )
    fpu_divide_latency = Param.Cycles(
        12, "Unpipelined FP32 divide latency in VPU cycles"
    )
    bank_read_ports = Param.UInt32(0, "Dedicated read ports per bank")
    bank_write_ports = Param.UInt32(0, "Dedicated write ports per bank")
    bank_read_write_ports = Param.UInt32(1, "Shared RW ports per bank")
    bank_access_entries = Param.UInt32(1, "Pending accesses per bank")
    bank_latency = Param.Cycles(1, "Bank response latency in VPU cycles")
    bank_initiation_interval = Param.Cycles(
        1, "Minimum VPU cycles between accesses on each bank port"
    )
    memory_max_block_bytes = Param.UInt32(
        64, "Maximum bytes per unit-stride block and block buffer"
    )
    memory_active_blocks = Param.UInt32(1, "One active unit-stride block")
    memory_vrf_inputs_per_lane = Param.UInt32(
        1, "Persistent VLSU word-access inputs per lane register file"
    )
    memory_max_outstanding_packets = Param.UInt32(1, "Physical packet limit")
    interconnect_model = Param.String(
        "crossbar", "Inter-lane transport model; only crossbar exists"
    )
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
    # SlideUnit: entradas al LRF y buffers por lane (vslide1up/down).
    slide_vrf_inputs_per_lane = Param.UInt32(
        1, "Persistent slide word-access inputs per lane register file"
    )
    slide_source_buffers_per_lane = Param.UInt32(
        1, "Read source words per lane until copied or accepted by network"
    )
    slide_assembly_buffers_per_lane = Param.UInt32(
        2, "Destination words per lane until WriteAck; at least two"
    )
    address_bits = Param.Unsigned("Target address width: 32 or 64")
    target_byte_order = Param.ByteOrder("Target CPU byte order")

    mem_side = RequestPort("Vector memory timing requests and responses")

    @classmethod
    def from_configuration(cls, configuration, **kwargs):
        """Crea la VPU a partir de una VpuConfiguration.

        kwargs permite añadir argumentos gem5, como el dominio de reloj.
        Un parámetro no puede aparecer también en configuration: sería
        ambiguo qué valor usar. El constructor C++ comprueba los tamaños y
        recursos; los puertos se conectan después en el script de simulación.
        """
        from m5.objects.configuration import VpuConfiguration

        if not isinstance(configuration, VpuConfiguration):
            raise TypeError("Expected a VpuConfiguration")
        values = configuration.parameters()
        # Cada ajuste debe venir de una sola fuente, sin sobrescribir valores.
        duplicates = values.keys() & kwargs.keys()
        if duplicates:
            raise ValueError(f"Duplicate VPU settings: {sorted(duplicates)}")
        return cls(**values, **kwargs)
