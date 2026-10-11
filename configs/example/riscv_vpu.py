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

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--no-offload", action="store_true")
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
    system.cpu.vectorEngine = VectorEngine(
        vlen_bytes=32,
        lane_word_bytes=8,
        num_lanes=2,
        banks_per_lane=2,
        supported_lmuls=[0],
        address_bits=64,
        target_byte_order="little",
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
