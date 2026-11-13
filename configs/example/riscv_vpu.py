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

"""Run a supplied RV64 SE executable on MinorCPU with optional VPU offload.

This example uses explicit SimObjects to connect the custom VectorEngine port.
It models functional ordering; its timing is not calibrated to Ara hardware.
"""

import argparse
import dataclasses
import sys
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    Process,
    RiscvISA,
    RiscvMinorCPU,
    Root,
    SEWorkload,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VectorEngine,
    VoltageDomain,
)
from m5.objects.configuration import (
    AluConfiguration,
    FpuConfiguration,
    InterconnectConfiguration,
    LaneConfiguration,
    SlideConfiguration,
    VpuConfiguration,
    VrfConfiguration,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--no-offload", action="store_true")
# Concurrencia de la VPU (fase 5 del hito 2). Los valores por defecto
# coinciden con VpuConfiguration; max_active_commands no puede superar
# command_queue_entries.
parser.add_argument(
    "--vpu-command-queue-entries",
    type=int,
    default=VpuConfiguration.command_queue_entries,
    help="VPU command capacity: reservations, pending and active commands",
)
parser.add_argument(
    "--vpu-max-active-commands",
    type=int,
    default=VpuConfiguration.max_active_commands,
    help="Independent VPU commands that may execute at the same time",
)
parser.add_argument(
    "--vpu-lane-task-entries",
    type=int,
    default=LaneConfiguration.task_entries,
    help="Fragment contexts per lane, shared by active commands",
)
# Unidades de cálculo (fase 6 del hito 2). Los valores por defecto coinciden
# con AluConfiguration y FpuConfiguration; los tiempos están en ciclos de
# la VPU y son parámetros del modelo, no latencias de Ara.
parser.add_argument(
    "--vpu-alus-per-lane",
    type=int,
    default=LaneConfiguration.alus_per_lane,
    help="Independent ALU instances per lane",
)
parser.add_argument(
    "--vpu-alu-multiply-latency",
    type=int,
    default=AluConfiguration.multiply_latency,
    help="Pipelined integer multiply latency",
)
parser.add_argument(
    "--vpu-alu-divide-latencies",
    type=int,
    nargs=4,
    default=list(AluConfiguration.divide_latencies),
    metavar=("SEW8", "SEW16", "SEW32", "SEW64"),
    help="Unpipelined integer divide/remainder latency per SEW",
)
parser.add_argument(
    "--vpu-fpu-add-multiply-latency",
    type=int,
    default=FpuConfiguration.add_multiply_latency,
    help="Pipelined FP add/multiply latency",
)
parser.add_argument(
    "--vpu-fpu-divide-latency",
    type=int,
    default=FpuConfiguration.divide_latency,
    help="Unpipelined FP32 divide latency",
)
parser.add_argument(
    "--vpu-alu-operand-entries",
    type=int,
    default=AluConfiguration.operand_entries_per_source,
    help="ALU-class operand buffers per source and lane",
)
parser.add_argument(
    "--vpu-alu-result-entries",
    type=int,
    default=AluConfiguration.result_entries,
    help="ALU-class result buffers per lane",
)
parser.add_argument(
    "--vpu-fpu-operand-entries",
    type=int,
    default=FpuConfiguration.operand_entries_per_source,
    help="FPU-class operand buffers per source and lane",
)
parser.add_argument(
    "--vpu-fpu-result-entries",
    type=int,
    default=FpuConfiguration.result_entries,
    help="FPU-class result buffers per lane",
)
# Red entre lanes y slides (fase 7 del hito 2). Los valores por defecto
# coinciden con InterconnectConfiguration y SlideConfiguration; cero en
# tamaños y anchos de banda usa el tamaño de palabra del VRF.
parser.add_argument(
    "--vpu-interconnect-transit-latency",
    type=int,
    default=InterconnectConfiguration.transit_latency,
    help="Crossbar transit cycles after destination service",
)
parser.add_argument(
    "--vpu-interconnect-packets-per-destination",
    type=int,
    default=InterconnectConfiguration.packets_per_destination,
    help="Accepted and undelivered packets per destination lane",
)
parser.add_argument(
    "--vpu-interconnect-bytes-per-cycle",
    type=int,
    default=InterconnectConfiguration.source_bytes_per_cycle,
    help="Crossbar source and destination bytes per cycle",
)
parser.add_argument(
    "--vpu-slide-source-buffers",
    type=int,
    default=SlideConfiguration.source_buffers_per_lane,
    help="Slide source word buffers per lane",
)
parser.add_argument(
    "--vpu-slide-assembly-buffers",
    type=int,
    default=SlideConfiguration.assembly_buffers_per_lane,
    help="Slide destination word buffers per lane; at least two",
)
# Estudios de recursos: cualquier campo de VpuConfiguration por su ruta y
# la ventana vectorial de Minor. Se aplican después de las opciones --vpu-*
# anteriores, así que prevalecen sobre ellas.
parser.add_argument(
    "--vpu-set",
    action="append",
    default=[],
    metavar="PATH=VALUE",
    help="Override a VpuConfiguration field by its dotted path, e.g. "
    "vrf.banks_per_lane=4 or lane.alu.minimum_latency=2; tuples take "
    "comma-separated values (repeatable)",
)
parser.add_argument(
    "--minor-vector-window",
    type=int,
    help="MinorCPU maxActiveVectorOffloads (default: CPU parameter)",
)
parser.add_argument(
    "--minor-vector-dispatch-width",
    type=int,
    help="MinorCPU vectorDispatchWidth (default: CPU parameter)",
)
# Contraste con Ara RTL: latencia de SimpleMemory, compartida por CPU y VPU.
parser.add_argument(
    "--mem-latency",
    help="SimpleMemory request-to-response latency, e.g. 5ns "
    "(default: SimpleMemory parameter)",
)
# Ancho de banda de SimpleMemory. Con un valor bajo la memoria rechaza
# peticiones mientras está ocupada y fuerza retries en los puertos.
parser.add_argument(
    "--mem-bandwidth",
    help="SimpleMemory bandwidth, e.g. 200MiB/s; a low value makes memory "
    "reject requests while busy (default: SimpleMemory parameter)",
)
# VLEN en bits, igual para la ISA de Minor y el VRF de la VPU.
parser.add_argument(
    "--vlen",
    type=int,
    default=256,
    help="VLEN in bits for the ISA and the VPU VRF (default: 256)",
)
parser.add_argument("binary", type=Path, help="RV64 SE executable")
parser.add_argument("arguments", nargs=argparse.REMAINDER)
args = parser.parse_args()
binary = args.binary.expanduser().resolve()
if not binary.is_file():
    parser.error(f"Executable does not exist: {binary}")
for name in ("minor_vector_window", "minor_vector_dispatch_width"):
    value = getattr(args, name)
    if value is not None and value < 1:
        parser.error(f"--{name.replace('_', '-')} must be at least 1")


def convert_override(current, text, path):
    """Convierte text al tipo del valor actual del campo.

    Sólo se admiten los tipos que usan las configuraciones: int, str, bool y
    tuplas de int. Un error de formato se informa con la ruta completa.
    """
    try:
        if isinstance(current, bool):
            if text.lower() not in ("true", "false", "1", "0"):
                raise ValueError(text)
            return text.lower() in ("true", "1")
        if isinstance(current, int):
            return int(text, 0)
        if isinstance(current, str):
            return text
        if isinstance(current, tuple):
            return tuple(int(item, 0) for item in text.split(",") if item)
    except ValueError:
        parser.error(f"--vpu-set {path}: invalid value '{text}'")
    parser.error(f"--vpu-set {path}: unsupported field type")


def apply_override(config, path, text, full_path=None):
    """Devuelve una copia de config con el campo de path sustituido.

    Las configuraciones son dataclasses congeladas: se reconstruye con
    dataclasses.replace cada nivel de la ruta, de dentro hacia fuera.
    full_path conserva la ruta original para los mensajes de error.
    """
    if full_path is None:
        full_path = path
    head, _, rest = path.partition(".")
    if not dataclasses.is_dataclass(config) or head not in {
        item.name for item in dataclasses.fields(config)
    }:
        parser.error(f"--vpu-set {full_path}: unknown field '{head}'")
    current = getattr(config, head)
    if rest:
        value = apply_override(current, rest, text, full_path)
    elif dataclasses.is_dataclass(current):
        parser.error(f"--vpu-set {full_path}: names a group, not a field")
    else:
        value = convert_override(current, text, full_path)
    return dataclasses.replace(config, **{head: value})


def apply_overrides(config, overrides):
    """Aplica las sustituciones PATH=VALUE en el orden recibido."""
    for override in overrides:
        path, separator, text = override.partition("=")
        if not separator or not path:
            parser.error(f"--vpu-set expects PATH=VALUE, got '{override}'")
        config = apply_override(config, path, text)
    return config


system = System()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]
system.cpu = RiscvMinorCPU(
    isa=[RiscvISA(riscv_type="RV64", vlen=args.vlen, elen=64, enable_rvv=True)]
)
system.cpu.vectorOffloadEnabled = not args.no_offload
if args.minor_vector_window is not None:
    system.cpu.maxActiveVectorOffloads = args.minor_vector_window
if args.minor_vector_dispatch_width is not None:
    system.cpu.vectorDispatchWidth = args.minor_vector_dispatch_width
system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.system_port = system.membus.cpu_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0])
if args.mem_latency is not None:
    system.memory.latency = args.mem_latency
if args.mem_bandwidth is not None:
    system.memory.bandwidth = args.mem_bandwidth
system.memory.port = system.membus.mem_side_ports

if not args.no_offload:
    vpu_config = VpuConfiguration(
        vrf=VrfConfiguration(vlen_bytes=args.vlen // 8),
        lane=LaneConfiguration(
            task_entries=args.vpu_lane_task_entries,
            alus_per_lane=args.vpu_alus_per_lane,
            alu=AluConfiguration(
                operand_entries_per_source=args.vpu_alu_operand_entries,
                result_entries=args.vpu_alu_result_entries,
                multiply_latency=args.vpu_alu_multiply_latency,
                divide_latencies=tuple(args.vpu_alu_divide_latencies),
            ),
            fpu=FpuConfiguration(
                operand_entries_per_source=args.vpu_fpu_operand_entries,
                result_entries=args.vpu_fpu_result_entries,
                add_multiply_latency=args.vpu_fpu_add_multiply_latency,
                divide_latency=args.vpu_fpu_divide_latency,
            ),
        ),
        interconnect=InterconnectConfiguration(
            packets_per_destination=(
                args.vpu_interconnect_packets_per_destination
            ),
            source_bytes_per_cycle=args.vpu_interconnect_bytes_per_cycle,
            destination_bytes_per_cycle=(
                args.vpu_interconnect_bytes_per_cycle
            ),
            transit_latency=args.vpu_interconnect_transit_latency,
        ),
        slide=SlideConfiguration(
            source_buffers_per_lane=args.vpu_slide_source_buffers,
            assembly_buffers_per_lane=args.vpu_slide_assembly_buffers,
        ),
        command_queue_entries=args.vpu_command_queue_entries,
        max_active_commands=args.vpu_max_active_commands,
    )
    vpu_config = apply_overrides(vpu_config, args.vpu_set)
    system.cpu.vectorEngine = VectorEngine.from_configuration(vpu_config)
    system.cpu.vectorEngine.mem_side = system.membus.cpu_side_ports

system.workload = SEWorkload.init_compatible(str(binary))
process = Process()
process.cmd = [str(binary), *args.arguments]
system.cpu.workload = process
system.cpu.createThreads()
system.cpu.createInterruptController()
root = Root(full_system=False, system=system)
m5.instantiate()
exit_event = m5.simulate()
print(f"Exiting at tick {m5.curTick()}: {exit_event.getCause()}")
sys.exit(exit_event.getCode())
