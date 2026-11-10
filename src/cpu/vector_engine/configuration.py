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

"""Valores agrupados por módulo para describir cómo se construirá la VPU.

Estas clases guardan las elecciones de configuración. VectorEngine crea
los recursos y comprueba que los valores se pueden usar juntos en C++.
"""

from dataclasses import (
    dataclass,
    field,
    fields,
)
from typing import Tuple


@dataclass(frozen=True)
class VrfConfiguration:
    """Tamaño del VRF y forma de repartir sus palabras entre lanes y bancos."""

    vlen_bytes: int
    lane_word_bytes: int = 8
    num_lanes: int = 2
    banks_per_lane: int = 2
    mapping_policy: str = "word_striped"


@dataclass(frozen=True)
class ExecutionConfiguration:
    """Recursos comunes de una clase de unidad de cálculo en cada lane.

    width_bytes limita los bytes de cada bundle y se mantiene igual a la
    palabra del VRF. capacity cuenta bundles aceptados sin resultado
    entregado, por instancia. operand_entries_per_source y result_entries
    son los buffers que la lane reserva para esta clase; una clase ocupada
    no retiene los de la otra, como las colas por unidad de Ara. Los buffers
    de cada fuente se reservan por parejas; el escalar ocupa un RHS
    replicado sin leer VRF. Un resultado se reserva hasta WriteAck.
    """

    width_bytes: int = 0  # Cero usa el tamaño de palabra del VRF.
    capacity: int = 1
    operand_entries_per_source: int = 1
    result_entries: int = 1


@dataclass(frozen=True)
class AluConfiguration(ExecutionConfiguration):
    """ALU entera: perfil temporal por operación, en ciclos de la VPU.

    Suma, multiplicación y mínimo son segmentados. División y resto usan un
    divisor no segmentado: su intervalo de iniciación es su latencia, que
    se da por SEW en el orden 8/16/32/64. Son parámetros del modelo, no
    latencias de hardware verificadas.
    """

    add_latency: int = 1
    add_initiation_interval: int = 1
    multiply_latency: int = 3
    multiply_initiation_interval: int = 1
    minimum_latency: int = 1
    minimum_initiation_interval: int = 1
    divide_latencies: Tuple[int, ...] = (10, 18, 34, 66)


@dataclass(frozen=True)
class FpuConfiguration(ExecutionConfiguration):
    """FPU: pipeline de suma/multiplicación y divisor FP32 no segmentado.

    capacity se aplica al pipeline; el divisor admite un bundle cada vez.
    Son parámetros del modelo, no latencias de hardware verificadas.
    """

    add_multiply_latency: int = 4
    add_multiply_initiation_interval: int = 1
    divide_latency: int = 12


@dataclass(frozen=True)
class LaneConfiguration:
    """Recursos por lane para fragmentos de las instrucciones activas.

    task_entries limita contextos hasta confirmar writeback, compartidos por
    ALUs y FPU. alus_per_lane crea instancias de ALU independientes con la
    misma configuración; no cambia el ancho de cada bundle.
    """

    task_entries: int = 4
    max_bundle_bytes: int = 0
    alus_per_lane: int = 1
    alu: AluConfiguration = field(default_factory=AluConfiguration)
    fpu: FpuConfiguration = field(default_factory=FpuConfiguration)


@dataclass(frozen=True)
class BankConfiguration:
    """Accesos que puede atender un banco y sus tiempos en ciclos.

    Cada R/W sirve su dirección y cada RW sirve una sola dirección por
    oportunidad. initiation_interval se aplica por puerto; latency fija el
    retorno de cada acceso concedido. access_entries acota accesos en curso
    por banco, compartidos entre todos sus puertos. Se necesitan caminos de
    lectura y escritura. Los candidatos denegados permanecen en el emisor.
    """

    read_ports: int = 0
    write_ports: int = 0
    read_write_ports: int = 1
    access_entries: int = 1
    latency: int = 1
    initiation_interval: int = 1


@dataclass(frozen=True)
class VlsuConfiguration:
    """Bloques unit-stride y entradas de la VLSU en cada LRF.

    max_block_bytes limita cada bloque y su buffer, en bytes; debe contener
    elementos enteros de cada SEW habilitado. Los bloques se recortan
    además por línea, página y final del rango. vrf_inputs_per_lane fija
    cuántos accesos por palabra puede ofrecer la VLSU a la vez a cada LRF.
    """

    max_block_bytes: int = 64
    active_blocks: int = 1
    vrf_inputs_per_lane: int = 1


@dataclass(frozen=True)
class MemoryBackendConfiguration:
    """Tamaño de las direcciones y peticiones pendientes hacia memoria."""

    address_bits: int = 64
    max_outstanding_packets: int = 1


@dataclass(frozen=True)
class InterconnectConfiguration:
    """Modelo de red entre lanes, espacio, velocidad y tiempo de viaje.

    model elige la implementación al construir la VPU; sólo existe
    "crossbar". Los tamaños son bytes y las velocidades, bytes por ciclo;
    un cero usa lane_word_bytes. packets_per_destination y
    bytes_per_destination acotan los paquetes aceptados y no entregados
    hacia cada destino. transit_latency son ciclos tras el servicio.
    """

    model: str = "crossbar"
    max_packet_bytes: int = 0  # Cero usa lane_word_bytes.
    packets_per_destination: int = 1
    bytes_per_destination: int = 0
    source_bytes_per_cycle: int = 0
    destination_bytes_per_cycle: int = 0
    transit_latency: int = 1


@dataclass(frozen=True)
class SlideConfiguration:
    """Recursos de SlideUnit en cada lane para vslide1up y vslide1down.

    vrf_inputs_per_lane son las entradas que ofrece a cada LRF.
    source_buffers_per_lane son palabras fuente leídas hasta copiar o
    entregar a la red sus bytes. assembly_buffers_per_lane son palabras
    destino en construcción hasta su WriteAck; hacen falta al menos dos.
    """

    vrf_inputs_per_lane: int = 1
    source_buffers_per_lane: int = 1
    assembly_buffers_per_lane: int = 2


@dataclass(frozen=True)
class VpuConfiguration:
    """Reúne las elecciones para construir una VPU en una sola descripción.

    Cada parte describe un módulo y sus valores no cambian tras crearla.
    VectorEngine comprueba la composición al construir los recursos reales.
    Allí los anchos con valor cero toman el tamaño de palabra del VRF.

    El backend admite suma y memoria de 8/16/32/64 bits en el perfil SEW/LMUL
    de fase 2.2. supported_sews y supported_lmuls habilitan su intersección
    con ese perfil, no todas las parejas del producto cartesiano. La fase 3
    temporiza sus recursos locales y la fase 4 agrupa la memoria unit-stride
    en bloques, sin red. La fase 5 admite hasta max_active_commands
    comandos activos independientes; la memoria sigue siendo una barrera.
    La fase 6 añade Mul/Div/resto enteros, FP32 y varias ALUs por lane.
    La fase 7 añade la red entre lanes (crossbar) y las slides de un
    elemento sin predicación.
    LMUL usa exponentes de dos (-3 para mf8, 3 para m8).
    Los predeterminados conservan 16/32 bits y m1 y activan la concurrencia
    (cuatro comandos y cuatro contextos por lane); las rutas CPU se
    documentan por separado.
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
    slide: SlideConfiguration = field(default_factory=SlideConfiguration)
    supported_sews: Tuple[int, ...] = (16, 32)
    supported_lmuls: Tuple[int, ...] = (0,)
    elen_bits: int = 64
    command_queue_entries: int = 4
    max_active_commands: int = 4
    target_byte_order: str = "little"

    def parameters(self):
        """Convierte la descripción en los argumentos que espera VectorEngine.

        Por ejemplo, lane.alu.width_bytes se convierte en alu_width_bytes.
        Sólo cambia la organización de los valores: las comprobaciones se
        hacen al construir la VPU. Devuelve listas nuevas para que usarlas
        como parámetros no permita modificar esta descripción.
        """
        # Traducir los nombres que difieren entre módulos y parámetros gem5.
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
            "max_bundle_bytes": self.lane.max_bundle_bytes,
            "alus_per_lane": self.lane.alus_per_lane,
            "memory_max_block_bytes": self.vlsu.max_block_bytes,
            "memory_active_blocks": self.vlsu.active_blocks,
            "memory_vrf_inputs_per_lane": self.vlsu.vrf_inputs_per_lane,
            "address_bits": self.memory.address_bits,
            "memory_max_outstanding_packets": (
                self.memory.max_outstanding_packets
            ),
            "interconnect_model": self.interconnect.model,
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
        # Los campos de SlideUnit toman el prefijo slide_.
        for member in fields(self.slide):
            result[f"slide_{member.name}"] = getattr(self.slide, member.name)
        # Cada campo de la ALU o la FPU se convierte en el parámetro con el
        # prefijo de su unidad; las tuplas se copian como listas.
        for name in ("alu", "fpu"):
            unit = getattr(self.lane, name)
            for member in fields(unit):
                value = getattr(unit, member.name)
                if isinstance(value, tuple):
                    value = list(value)
                result[f"{name}_{member.name}"] = value
        # Los campos del banco siguen la misma regla, con el prefijo bank_.
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
