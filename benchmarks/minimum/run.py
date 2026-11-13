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

"""Mínimo entero vmin.vv y vminu.vv: compila y ejecuta en gem5.

Ejecuta vmin.vv (con signo) y vminu.vv (sin signo) con SEW 8/16/32/64 y
LMUL m1 y m4, con casos en los que ambos resultados difieren. La
configuración por defecto de riscv_vpu.py sólo habilita SEW 16/32 y m1;
este runner añade siempre --vpu-set supported_sews=8,16,32,64 y
supported_lmuls=0,2 en offload. Compila con make check, simula offload
(VPU) y native (RVV de Minor) y contrasta sus huellas. En offload exige
que la ALU haya calculado mínimos. Resultados en results/<fecha UTC>/.

Uso:
  python3 benchmarks/minimum/run.py                      # ambos modos
  python3 benchmarks/minimum/run.py --elements 300 -v    # salida detallada
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
    name="minimum",
    directory=Path(__file__).resolve().parent,
    title="Mínimo entero con y sin signo",
    roi_description="`runKernel()` (m1 y m4, los cuatro SEW)",
    options=(
        BuildOption(
            "elements",
            "ELEMENTS",
            150,
            "Elements per SEW; the first 10 are corner cases (default: 150)",
            type=int,
            positive=True,
        ),
    ),
    build_dir=lambda args: f"build/n{args.elements}",
    binary_name="minimum",
    config_args=(
        "--vpu-set=supported_sews=8,16,32,64",
        "--vpu-set=supported_lmuls=0,2",
    ),
    summary_metrics=(
        ("Elementos de mínimo (ALU)", ENGINE + "lane*.alu*.minimumElements"),
        ("ALUs ocupadas (ticks)", ENGINE + "lane*.alu*.busyTicks"),
    ),
    offload_checks=(vpu_bench.require_nonzero("lane*.alu*.minimumElements"),),
)

if __name__ == "__main__":
    sys.exit(vpu_bench.run_main(BENCHMARK, __doc__))
