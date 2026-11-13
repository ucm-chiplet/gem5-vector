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

"""Cadenas productor-consumidor (criterio 2 del hito 2): compila y ejecuta.

Ejecuta cuatro cadenas RAW entre unidades con latencias distintas (FPU,
divisor, ALU y SlideUnit) y un par independiente de control, con SEW 32
y m1. Compila con make check, simula offload (VPU) y native (RVV de
Minor) y contrasta sus huellas. En offload activa la traza VectorTimeline
y check_timeline.py exige que ningún consumidor empiece antes de que
termine su productor, y que el par de control sí se solape. La ventana
de Minor sube a 4 para que el consumidor espere en la VPU y no en Minor.
Resultados en results/<fecha UTC>/.

Uso:
  python3 benchmarks/dependencies/run.py                 # ambos modos
  python3 benchmarks/dependencies/run.py --elements 100 -v
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))

import check_timeline  # noqa: E402
import vpu_bench  # noqa: E402
from vpu_bench import (  # noqa: E402
    ENGINE,
    Benchmark,
    BuildOption,
)

BENCHMARK = Benchmark(
    name="dependencies",
    directory=Path(__file__).resolve().parent,
    title="Cadenas productor-consumidor",
    roi_description="`runKernel()`",
    options=(
        BuildOption(
            "elements",
            "ELEMENTS",
            60,
            "Elements; VLMAX is 8, so 60 gives 8 strips (default: 60)",
            type=int,
            positive=True,
        ),
    ),
    build_dir=lambda args: f"build/n{args.elements}",
    binary_name="dependencies",
    config_args=("--minor-vector-window=4",),
    gem5_args=("--debug-flags=VectorTimeline", "--debug-file=trace.out"),
    summary_metrics=(
        ("Espera por RAW (ticks)", ENGINE + "sequencer.rawWaitTicks"),
        ("Inicios solapados", ENGINE + "sequencer.overlappedStarts"),
    ),
    offload_checks=(
        vpu_bench.require_nonzero(
            "lane*.fpu*.multiply32Elements",
            "lane*.alu*.divideElements",
            "lane*.alu*.minimumElements",
            "sequencer.slideCommands",
        ),
    ),
    offload_run_checks=(check_timeline.check,),
)

if __name__ == "__main__":
    sys.exit(vpu_bench.run_main(BENCHMARK, __doc__))
