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

"""Anchos y grupos (criterios 3 y 10 del hito 2): compila y ejecuta en gem5.

Ejecuta vle/vadd.vv/vadd.vx/vse en las 22 combinaciones legales de SEW
8/16/32/64 y LMUL m1--m8 y mf2--mf8 (SEW <= LMUL * ELEN), con varias
tiras, tira final parcial, cruce de línea y de página y bytes de guarda
tras cada salida. La configuración por defecto de riscv_vpu.py sólo
habilita SEW 16/32 y m1; este runner añade siempre
--vpu-set supported_sews=8,16,32,64 y supported_lmuls=-3,...,3 en offload.
Compila con make check, simula offload (VPU) y native (RVV de Minor) y
contrasta sus huellas. Resultados en results/<fecha UTC>/.

Uso:
  python3 benchmarks/widths/run.py                       # ambos modos
  python3 benchmarks/widths/run.py --elements 500 -v     # salida detallada
  python3 benchmarks/widths/run.py --mode offload \\
      --config-arg=--vpu-set=vlsu.max_block_bytes=16
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
    name="widths",
    directory=Path(__file__).resolve().parent,
    title="Anchos y grupos",
    roi_description="`runKernel()` (las 22 combinaciones SEW/LMUL)",
    options=(
        BuildOption(
            "elements",
            "ELEMENTS",
            300,
            "Elements per combination; keep it above 256 (largest VLMAX) "
            "to get several strips (default: 300)",
            type=int,
            positive=True,
        ),
    ),
    build_dir=lambda args: f"build/n{args.elements}",
    binary_name="widths",
    config_args=(
        "--vpu-set=supported_sews=8,16,32,64",
        "--vpu-set=supported_lmuls=-3,-2,-1,0,1,2,3",
    ),
    summary_metrics=(
        ("Elementos de suma (ALU)", ENGINE + "lane*.alu*.addElements"),
        ("Comandos de load", ENGINE + "sequencer.loadCommands"),
        ("Comandos de store", ENGINE + "sequencer.storeCommands"),
        ("Bloques de memoria (VLSU)", ENGINE + "vlsu.blocks"),
        ("Bloques de un elemento partido", ENGINE + "vlsu.splitElementBlocks"),
        ("Paquetes físicos de memoria", ENGINE + "memory.packetsSent"),
    ),
    summary_notes=(
        "Los bloques de memoria se recortan por capacidad, línea y página:",
        "un comando que cruza una página genera al menos dos.",
    ),
    offload_checks=(
        vpu_bench.require_nonzero("lane*.alu*.addElements", "vlsu.blocks"),
    ),
)

if __name__ == "__main__":
    sys.exit(vpu_bench.run_main(BENCHMARK, __doc__))
