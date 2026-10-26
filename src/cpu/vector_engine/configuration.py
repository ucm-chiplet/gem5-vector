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

"""Immutable construction descriptions for the current VPU organization.

These are configuration values, not SimObjects or executable resources.
VectorEngine validates the translated values again on the C++ boundary.
"""

from dataclasses import (
    dataclass,
    field,
)
from typing import Tuple


@dataclass(frozen=True)
class VrfConfiguration:
    """Geometría en bytes/bancos/lanes y política del layout inicial."""

    vlen_bytes: int
    lane_word_bytes: int = 8
    num_lanes: int = 2
    banks_per_lane: int = 2
    mapping_policy: str = "word_striped"


@dataclass(frozen=True)
class ExecutionConfiguration:
    """Bytes por bundle, bundles pendientes y ciclos de latencia/iniciación."""

    width_bytes: int = 0  # Resolve to the VRF word width during construction.
    capacity: int = 1
    latency: int = 1  # Cycles; functional scheduling, not calibrated hardware.
    initiation_interval: int = 1


@dataclass(frozen=True)
class LaneConfiguration:
    """Contextos/buffers por lane y fuente; no activa varias instrucciones."""

    task_entries: int = 1
    operand_entries_per_source: int = 1
    result_entries: int = 1
    max_bundle_bytes: int = 0
    alus_per_lane: int = 1
    alu: ExecutionConfiguration = field(default_factory=ExecutionConfiguration)
    fpu: ExecutionConfiguration = field(default_factory=ExecutionConfiguration)


@dataclass(frozen=True)
class BankConfiguration:
    """Puertos y accesos por banco; el perfil actual exige un único RW."""

    read_ports: int = 0
    write_ports: int = 0
    read_write_ports: int = 1
    access_entries: int = 1
    latency: int = 1
    initiation_interval: int = 1


@dataclass(frozen=True)
class VlsuConfiguration:
    """Límites de memoria preparados sin habilitar agrupación de elementos."""

    max_block_bytes: int = 8
    active_blocks: int = 1
    block_requests: bool = False


@dataclass(frozen=True)
class MemoryBackendConfiguration:
    """Ancho de dirección y paquetes físicos, separados de bloques lógicos."""

    address_bits: int = 64
    max_outstanding_packets: int = 1


@dataclass(frozen=True)
class InterconnectConfiguration:
    """Capacidades de transporte declaradas; enabled debe seguir a False."""

    enabled: bool = False
    max_packet_bytes: int = 0  # Zero resolves to lane_word_bytes.
    packets_per_destination: int = 1
    bytes_per_destination: int = 0
    source_bytes_per_cycle: int = 0
    destination_bytes_per_cycle: int = 0
    transit_latency: int = 1


@dataclass(frozen=True)
class VpuConfiguration:
    """Composición de valores inmutables, validada al construir VectorEngine.

    No crea SimObjects ni recursos. Los ceros de anchura se resuelven en C++
    usando la palabra del VRF; describir capacidades no habilita su servicio.
    """

    vrf: VrfConfiguration
    lane: LaneConfiguration = field(default_factory=LaneConfiguration)
    bank: BankConfiguration = field(default_factory=BankConfiguration)
    vlsu: VlsuConfiguration = field(default_factory=VlsuConfiguration)
    memory: MemoryBackendConfiguration = field(
        default_factory=MemoryBackendConfiguration
    )
    interconnect: InterconnectConfiguration = field(
        default_factory=InterconnectConfiguration
    )
    supported_sews: Tuple[int, ...] = (16, 32)
    supported_lmuls: Tuple[int, ...] = (0,)
    elen_bits: int = 64
    command_queue_entries: int = 1
    max_active_commands: int = 1
    target_byte_order: str = "little"

    def parameters(self):
        """Devuelve una copia plana para el ABI de parámetros generado por gem5.

        Cada módulo conserva aquí sus unidades y nombres. Este método sólo
        traduce: no consulta estado CPU, no resuelve anchos ni valida soporte.
        Las listas devueltas son copias y no modifican esta configuración.
        """
        result = {
            "vlen_bytes": self.vrf.vlen_bytes,
            "lane_word_bytes": self.vrf.lane_word_bytes,
            "num_lanes": self.vrf.num_lanes,
            "banks_per_lane": self.vrf.banks_per_lane,
            "mapping_policy": self.vrf.mapping_policy,
            "supported_sews": list(self.supported_sews),
            "supported_lmuls": list(self.supported_lmuls),
            "elen_bits": self.elen_bits,
            "command_queue_entries": self.command_queue_entries,
            "max_active_commands": self.max_active_commands,
            "target_byte_order": self.target_byte_order,
            "lane_task_entries": self.lane.task_entries,
            "operand_buffer_entries_per_source": (
                self.lane.operand_entries_per_source
            ),
            "result_buffer_entries": self.lane.result_entries,
            "max_bundle_bytes": self.lane.max_bundle_bytes,
            "alus_per_lane": self.lane.alus_per_lane,
            "memory_max_block_bytes": self.vlsu.max_block_bytes,
            "memory_active_blocks": self.vlsu.active_blocks,
            "memory_block_requests": self.vlsu.block_requests,
            "address_bits": self.memory.address_bits,
            "memory_max_outstanding_packets": (
                self.memory.max_outstanding_packets
            ),
            "interconnect_enabled": self.interconnect.enabled,
            "interconnect_max_packet_bytes": self.interconnect.max_packet_bytes,
            "interconnect_packets_per_destination": (
                self.interconnect.packets_per_destination
            ),
            "interconnect_bytes_per_destination": (
                self.interconnect.bytes_per_destination
            ),
            "interconnect_source_bytes_per_cycle": (
                self.interconnect.source_bytes_per_cycle
            ),
            "interconnect_destination_bytes_per_cycle": (
                self.interconnect.destination_bytes_per_cycle
            ),
            "interconnect_transit_latency": self.interconnect.transit_latency,
        }
        for name in ("alu", "fpu"):
            unit = getattr(self.lane, name)
            for member in (
                "width_bytes",
                "capacity",
                "latency",
                "initiation_interval",
            ):
                result[f"{name}_{member}"] = getattr(unit, member)
        for member in (
            "read_ports",
            "write_ports",
            "read_write_ports",
            "access_entries",
            "latency",
            "initiation_interval",
        ):
            result[f"bank_{member}"] = getattr(self.bank, member)
        return result
