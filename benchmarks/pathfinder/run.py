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

"""Compila y ejecuta Pathfinder en gem5 con VPU y con RVV nativo.

Usa el runner común de ``benchmarks/common/vpu_bench.py``: compila con
``make check`` (que comprueba el perfil RVV), simula los modos
``offload`` (VPU) y ``native`` (RVV de MinorCPU) y contrasta sus huellas.
El primer volcado de ``stats.txt`` cubre sólo ``runVector()``. Los
resultados se guardan en ``results/<fecha>/``, ignorado por Git.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))

import vpu_bench  # noqa: E402
from vpu_bench import (  # noqa: E402
    ENGINE,
    Benchmark,
    BuildOption,
)

BENCHMARK = Benchmark(
    name="pathfinder",
    directory=Path(__file__).resolve().parent,
    title="Pathfinder",
    roi_description="`runVector()`",
    options=(
        BuildOption(
            "input",
            "INPUT",
            "tiny",
            "Input from input/data_<name>.in",
            choices=("tiny", "small"),
        ),
        BuildOption(
            "runs", "NUM_RUNS", 1, "NUM_RUNS", type=int, positive=True
        ),
    ),
    build_dir=lambda args: f"build/{args.input}-runs{args.runs}",
    binary_name="pathfinder",
    summary_metrics=(
        ("Elementos de mínimo (ALU)", ENGINE + "lane*.alu*.minimumElements"),
        ("Elementos de suma (ALU)", ENGINE + "lane*.alu*.addElements"),
        ("Tareas slide1up", ENGINE + "slide.slide1UpTasks"),
        ("Tareas slide1down", ENGINE + "slide.slide1DownTasks"),
        ("Paquetes de red locales", ENGINE + "interconnect.localPackets"),
        ("Paquetes de red remotos", ENGINE + "interconnect.remotePackets"),
        ("Comandos de load", ENGINE + "sequencer.loadCommands"),
        ("Comandos de store", ENGINE + "sequencer.storeCommands"),
        ("Peticiones lógicas de load", ENGINE + "memory.loads"),
        ("Peticiones lógicas de store", ENGINE + "memory.stores"),
        ("Paquetes físicos de memoria", ENGINE + "memory.packetsSent"),
    ),
    sweep_metrics=(
        ("Slide ocupada", ENGINE + "slide.busyTicks"),
        ("Red: transporte", ENGINE + "interconnect.transportTicks"),
        ("Red: remotos", ENGINE + "interconnect.remotePackets"),
    ),
    default_sweep=(
        {"label": "base", "args": []},
        {"label": "alus2", "args": ["--vpu-set=lane.alus_per_lane=2"]},
        {"label": "banks4", "args": ["--vpu-set=vrf.banks_per_lane=4"]},
        {"label": "rw-ports2", "args": ["--vpu-set=bank.read_write_ports=2"]},
        {"label": "lanes4", "args": ["--vpu-set=vrf.num_lanes=4"]},
        {
            "label": "transit4",
            "args": ["--vpu-set=interconnect.transit_latency=4"],
        },
        {"label": "block32", "args": ["--vpu-set=vlsu.max_block_bytes=32"]},
        {"label": "window4", "args": ["--minor-vector-window=4"]},
        {"label": "active1", "args": ["--vpu-set=max_active_commands=1"]},
    ),
    summary_notes=(
        "Las peticiones lógicas de memoria son bloques de la VLSU, que se",
        "recortan por capacidad, línea o página: un comando puede generar "
        "varias.",
    ),
)

if __name__ == "__main__":
    sys.exit(vpu_bench.run_main(BENCHMARK, __doc__))
