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
# Red entre lanes (fase 7 del hito 2). Los valores por defecto
# coinciden con InterconnectConfiguration; cero en
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
parser.add_argument("binary", type=Path, help="RV64 SE executable")
parser.add_argument("arguments", nargs=argparse.REMAINDER)
args = parser.parse_args()
binary = args.binary.expanduser().resolve()
if not binary.is_file():
    parser.error(f"Executable does not exist: {binary}")

system = System()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]
system.cpu = RiscvMinorCPU(
    isa=[RiscvISA(riscv_type="RV64", vlen=256, elen=64, enable_rvv=True)]
)
system.cpu.vectorOffloadEnabled = not args.no_offload
system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.system_port = system.membus.cpu_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.memory.port = system.membus.mem_side_ports

if not args.no_offload:
    system.cpu.vectorEngine = VectorEngine.from_configuration(
        VpuConfiguration(
            vrf=VrfConfiguration(vlen_bytes=32),
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
            command_queue_entries=args.vpu_command_queue_entries,
            max_active_commands=args.vpu_max_active_commands,
        )
    )
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
