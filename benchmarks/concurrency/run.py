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

"""Compila y ejecuta el micro-kernel de concurrencia en gem5.

Usa el runner común de ``benchmarks/common/vpu_bench.py``: compila con
``make check`` (que comprueba el perfil RVV), simula los modos
``offload`` (VPU) y ``native`` (RVV de MinorCPU) y contrasta sus huellas.
El primer volcado de ``stats.txt`` cubre sólo ``runKernel()``. Los
resultados se guardan en ``results/<fecha>/``, ignorado por Git.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))

import vpu_bench  # noqa: E402
from vpu_bench import (  # noqa: E402
    CPU,
    ENGINE,
    Benchmark,
    BuildOption,
)

BENCHMARK = Benchmark(
    name="concurrency",
    directory=Path(__file__).resolve().parent,
    title="Concurrencia aritmética",
    roi_description="`runKernel()`",
    options=(
        BuildOption(
            "variant",
            "VARIANT",
            "integer",
            "integer: vadd/vmul/vmin; mixed: vadd/vmin with vfadd/vfmul",
            choices=("integer", "mixed"),
        ),
        BuildOption(
            "elements",
            "ELEMENTS",
            1024,
            "Elements per array",
            type=int,
            positive=True,
        ),
    ),
    build_dir=lambda args: f"build/{args.variant}-n{args.elements}",
    binary_name="concurrency",
    summary_metrics=(
        (
            "Comandos aritméticos iniciados",
            ENGINE + "sequencer.arithmeticCommands",
        ),
        ("Comandos FPU (Minor)", CPU + "vectorFpuCommands"),
        (
            "Espera por dependencias (ticks)",
            ENGINE + "sequencer.hazardWaitTicks",
        ),
        (
            "Espera por capacidad (ticks)",
            ENGINE + "sequencer.capacityWaitTicks",
        ),
        (
            "Espera en barrera de memoria (ticks)",
            ENGINE + "sequencer.memoryBarrierWaitTicks",
        ),
        (
            "Bloqueos de ventana Minor: capacidad",
            CPU + "vectorBlockedCapacity",
        ),
        (
            "Bloqueos de ventana Minor: estructurales",
            CPU + "vectorBlockedStructural",
        ),
        ("ALUs ocupadas (ticks)", ENGINE + "lane*.alu*.busyTicks"),
        ("FPUs ocupadas (ticks)", ENGINE + "lane*.fpu.busyTicks"),
        ("Espera de VRF (ticks)", ENGINE + "lrf*.grantWaitTicks"),
    ),
    sweep_metrics=(
        ("FPUs ocupadas", ENGINE + "lane*.fpu.busyTicks"),
        ("Espera por capacidad", ENGINE + "sequencer.capacityWaitTicks"),
    ),
    default_sweep=(
        {"label": "base", "args": []},
        {"label": "alus2", "args": ["--vpu-set=lane.alus_per_lane=2"]},
        {"label": "rw-ports2", "args": ["--vpu-set=bank.read_write_ports=2"]},
        {"label": "banks4", "args": ["--vpu-set=vrf.banks_per_lane=4"]},
        {"label": "window4", "args": ["--minor-vector-window=4"]},
        {
            "label": "window4-alus2",
            "args": [
                "--minor-vector-window=4",
                "--vpu-set=lane.alus_per_lane=2",
            ],
        },
        {"label": "active1", "args": ["--vpu-set=max_active_commands=1"]},
    ),
)

if __name__ == "__main__":
    sys.exit(vpu_bench.run_main(BENCHMARK, __doc__))
