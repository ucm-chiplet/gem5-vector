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

"""Ejemplos al cierre del hito 2: compila y ejecuta en gem5.

Ejecuta los dos fragmentos de "Ejemplos al cierre: enteros de 16 bits y
FP32" del plan de evolución (variante int16: vadd, vmul y vslide1down con
SEW=16; variante fp32: vfmul y vfadd), con el caso del documento y un
caso pseudoaleatorio de varias tiras. Compila con make check (que
comprueba el perfil RVV), simula offload (VPU) y native (RVV de Minor) y
contrasta sus huellas. En offload exige además que la ALU, la SlideUnit o
la FPU hayan hecho el trabajo. Resultados en results/<fecha UTC>/.

Uso:
  python3 benchmarks/examples/run.py                    # int16, ambos modos
  python3 benchmarks/examples/run.py --variant fp32
  python3 benchmarks/examples/run.py --variant int16 --elements 37 -v
  python3 benchmarks/examples/run.py --mode offload \\
      --config-arg=--vpu-set=vrf.num_lanes=4
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

INT16_CHECK = vpu_bench.require_nonzero(
    "lane*.alu*.addElements",
    "lane*.alu*.multiplyElements",
    "slide.slide1DownTasks",
)
FP32_CHECK = vpu_bench.require_nonzero(
    "lane*.fpu.add32Elements", "lane*.fpu.multiply32Elements"
)


def check_variant(stats):
    """Elige las unidades exigidas según la variante compilada.

    Las comprobaciones reciben sólo las estadísticas; la variante se
    deduce de qué unidad tiene trabajo: si la FPU no tiene ninguno, se
    exige el de int16.
    """
    fpu_work = vpu_bench.metric(stats, ENGINE + "lane*.fpu.add32Elements")
    if fpu_work:
        return FP32_CHECK(stats)
    return INT16_CHECK(stats)


BENCHMARK = Benchmark(
    name="examples",
    directory=Path(__file__).resolve().parent,
    title="Ejemplos al cierre del hito 2",
    roi_description="`runKernel()` (caso del documento y caso aleatorio)",
    options=(
        BuildOption(
            "variant",
            "VARIANT",
            "int16",
            "int16: vadd/vmul/vslide1down with SEW=16; "
            "fp32: vfmul/vfadd with SEW=32",
            choices=("int16", "fp32"),
        ),
        BuildOption(
            "elements",
            "ELEMENTS",
            100,
            "Elements of the pseudo-random case (default: 100)",
            type=int,
            positive=True,
        ),
    ),
    build_dir=lambda args: f"build/{args.variant}-n{args.elements}",
    binary_name="examples",
    summary_metrics=(
        ("Elementos de suma (ALU)", ENGINE + "lane*.alu*.addElements"),
        (
            "Elementos de producto (ALU)",
            ENGINE + "lane*.alu*.multiplyElements",
        ),
        ("Tareas slide1down", ENGINE + "slide.slide1DownTasks"),
        ("Paquetes de red remotos", ENGINE + "interconnect.remotePackets"),
        ("Elementos de suma FP32", ENGINE + "lane*.fpu.add32Elements"),
        (
            "Elementos de producto FP32",
            ENGINE + "lane*.fpu.multiply32Elements",
        ),
        ("Bloques de memoria (VLSU)", ENGINE + "vlsu.blocks"),
    ),
    offload_checks=(check_variant,),
)

if __name__ == "__main__":
    sys.exit(vpu_bench.run_main(BENCHMARK, __doc__))
