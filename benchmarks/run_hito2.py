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

"""Regresión de cierre del hito 2: ejecuta todas las cargas de la VPU.

Lanza el run.py de cada caso en modo offload (MinorCPU con VPU) y native
(RVV de MinorCPU) con la configuración por defecto de riscv_vpu.py, más las
opciones fijas que cada benchmark declara (por ejemplo, habilitar SEW 8 y
64). Un caso pasa si su run.py termina con código 0: compilación y perfil
RVV correctos, PASS en cada modo, huellas iguales y, en offload, trabajo
real de la VPU en las unidades esperadas. En faults, el resultado esperado
es el panic de gem5 con el fault y el vstart correctos (ver su README).

Casos (en este orden; --list los muestra):
  baseline          hito 1: 21 casos vadd.vv/vadd.vx SEW 32
  baseline-simple   hito 1: ejemplo simple
  examples-int16    ejemplo al cierre: vadd/vmul/vslide1down SEW 16
  examples-fp32     ejemplo al cierre: vfmul/vfadd FP32
  widths            criterios 3 y 10: suma y memoria, 22 SEW/LMUL
  muldiv            criterio 13: Mul/Div/resto .vv y .vx, SEW 8-64
  concurrency-int   criterio 1: aritmética entera independiente
  concurrency-mixed criterios 1 y 4: ALU y FPU a la vez
  pathfinder        criterio 16: Pathfinder tiny
  minimum           vmin.vv y vminu.vv, SEW 8-64, m1 y m4
  dependencies      criterio 2: cadenas productor-consumidor
  widths-retry      criterio 11: widths con retries de memoria forzados
  faults            criterio 8: fault de página a mitad de vle32/vse32

Los resultados quedan en benchmarks/results/hito2-<fecha UTC>/ (ignorado
por Git): un directorio por caso con los resultados de su run.py, su
salida en <caso>.log y un summary.md con la tabla de todos los casos.
Devuelve 0 sólo si todos los casos pasan.

Uso:
  python3 benchmarks/run_hito2.py                 # todos, uno tras otro
  python3 benchmarks/run_hito2.py -j 4            # cuatro casos a la vez
  python3 benchmarks/run_hito2.py -v              # salida detallada
  python3 benchmarks/run_hito2.py --only widths --only muldiv
  python3 benchmarks/run_hito2.py --mode offload --list
"""

import argparse
import json
import sys
import time
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)
from pathlib import Path

BENCHMARKS = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCHMARKS / "common"))

import vpu_bench  # noqa: E402

# (etiqueta, script relativo a benchmarks/, argumentos, descripción).
# Las opciones de compilación son las de cada run.py por defecto salvo las
# indicadas; los README de cada benchmark explican qué comprueban.
CASES = (
    (
        "baseline",
        "baseline/run.py",
        ["--program=baseline"],
        "Hito 1: 21 casos vadd.vv/vadd.vx con SEW 32",
    ),
    (
        "baseline-simple",
        "baseline/run.py",
        ["--program=baseline-simple"],
        "Hito 1: ejemplo simple A + B + 5",
    ),
    (
        "examples-int16",
        "examples/run.py",
        ["--variant=int16"],
        "Ejemplo al cierre: vadd/vmul/vslide1down con SEW 16",
    ),
    (
        "examples-fp32",
        "examples/run.py",
        ["--variant=fp32"],
        "Ejemplo al cierre: vfmul/vfadd FP32",
    ),
    (
        "widths",
        "widths/run.py",
        [],
        "Criterios 3 y 10: suma y memoria en 22 combinaciones SEW/LMUL",
    ),
    (
        "muldiv",
        "muldiv/run.py",
        [],
        "Criterio 13: vmul/vdiv/vdivu/vrem/vremu .vv y .vx, SEW 8-64",
    ),
    (
        "concurrency-int",
        "concurrency/run.py",
        ["--variant=integer"],
        "Criterio 1: cuatro operaciones enteras independientes",
    ),
    (
        "concurrency-mixed",
        "concurrency/run.py",
        ["--variant=mixed"],
        "Criterios 1 y 4: ALU y FPU progresando a la vez",
    ),
    (
        "pathfinder",
        "pathfinder/run.py",
        ["--input=tiny"],
        "Criterio 16: Pathfinder (RiVEC), entrada tiny",
    ),
    (
        "minimum",
        "minimum/run.py",
        [],
        "vmin.vv y vminu.vv en SEW 8-64, m1 y m4",
    ),
    (
        "dependencies",
        "dependencies/run.py",
        [],
        "Criterio 2: ningún consumidor empieza antes que su productor",
    ),
    (
        "widths-retry",
        "widths/run.py",
        [
            "--config-arg=--mem-bandwidth=200MiB/s",
            "--require-nonzero=memory.portRetries",
        ],
        "Criterio 11: widths con la memoria rechazando peticiones",
    ),
    (
        "faults",
        "faults/run.py",
        [],
        "Criterio 8: fault de página a mitad de vle32/vse32 y vstart",
    ),
)


def run_case(case, args, out_dir):
    """Ejecuta un caso y devuelve su resultado.

    La salida del run.py hijo se guarda en <caso>.log; con --verbose se
    muestra también, con la etiqueta del caso delante de cada línea para
    distinguirla cuando hay varios casos en paralelo.
    """
    label, script, extra, description = case
    command = [
        sys.executable,
        str(BENCHMARKS / script),
        *extra,
        f"--mode={args.mode}",
        f"--gem5={args.gem5}",
        f"--timeout={args.timeout}",
        f"--results-dir={out_dir / label}",
    ]
    if args.verbose:
        command.append("--verbose")
    start = time.monotonic()
    status, _ = vpu_bench.run_logged(
        command,
        out_dir / f"{label}.log",
        verbose=args.verbose,
        prefix=f"[{label}] ",
    )
    result = {
        "label": label,
        "description": description,
        "command": command,
        "returncode": status,
        "passed": status == 0,
        "seconds": round(time.monotonic() - start, 1),
        "errors": [],
    }
    # run.py deja su informe en results.json; de él salen los errores.
    report_path = out_dir / label / "results.json"
    if report_path.is_file():
        report = json.loads(report_path.read_text())
        if report.get("build_returncode") not in (0, None):
            result["errors"].append("compilación o perfil RVV fallidos")
        for mode, run in report.get("runs", {}).items():
            if not run.get("passed", False):
                errors = "; ".join(run.get("errors", [])) or "FAIL"
                result["errors"].append(f"{mode}: {errors}")
        if report.get("checksum_error"):
            result["errors"].append(report["checksum_error"])
    elif not result["passed"]:
        result["errors"].append(f"sin results.json; ver {label}.log")
    return result


def write_summary(results, args, out_dir):
    """Escribe summary.md y results.json con todos los casos."""
    passed = all(result["passed"] for result in results)
    lines = [
        "# Regresión de cierre del hito 2",
        "",
        f"Resultado global: **{'PASS' if passed else 'FAIL'}**.",
        "",
        f"Commit `{vpu_bench.git_commit()}`, modo `{args.mode}`.",
        "",
        "| Caso | Qué comprueba | Resultado | Segundos | Errores |",
        "| --- | --- | --- | --- | --- |",
    ]
    for result in results:
        lines.append(
            f"| `{result['label']}` | {result['description']} | "
            f"{'PASS' if result['passed'] else 'FAIL'} | "
            f"{result['seconds']} | "
            f"{'; '.join(result['errors']) or '-'} |"
        )
    lines.extend(
        [
            "",
            "Cada caso tiene su directorio con `summary.md`, registros y",
            "estadísticas de su run.py, y su salida completa en `<caso>.log`.",
            "",
        ]
    )
    (out_dir / "summary.md").write_text("\n".join(lines))
    (out_dir / "results.json").write_text(
        json.dumps(
            {"passed": passed, "mode": args.mode, "cases": results},
            indent=2,
            ensure_ascii=False,
        )
        + "\n"
    )
    return passed


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    labels = [case[0] for case in CASES]
    parser.add_argument(
        "--only",
        action="append",
        choices=labels,
        default=[],
        metavar="CASE",
        help="Run only this case (repeatable); see --list",
    )
    parser.add_argument(
        "--list", action="store_true", help="List the cases and exit"
    )
    parser.add_argument(
        "--mode",
        choices=("both", "offload", "native"),
        default="both",
        help="Modes passed to every run.py (default: both)",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=1,
        help="Cases run in parallel (default: 1)",
    )
    parser.add_argument(
        "--gem5",
        type=Path,
        default=vpu_bench.DEFAULT_GEM5,
        help="gem5 binary (default: build/RISCV/gem5.opt)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=900,
        help="Maximum wall-clock seconds per simulation (default: 900)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Print build, gem5 and program output of every case, prefixed "
        "with its label, and the kernel metrics of each run.py",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        help="Empty or new directory; default: "
        "benchmarks/results/hito2-<UTC date>",
    )
    args = parser.parse_args()

    cases = [case for case in CASES if not args.only or case[0] in args.only]
    if args.list:
        for label, script, extra, description in cases:
            print(f"{label:18} {description}")
            print(f"{'':18} {script} {' '.join(extra)}".rstrip())
        return 0
    if args.jobs < 1:
        parser.error("--jobs debe ser al menos 1")
    if args.timeout <= 0:
        parser.error("--timeout debe ser positivo")
    args.gem5 = args.gem5.expanduser().resolve()
    if not args.gem5.is_file():
        parser.error(f"gem5 no existe: {args.gem5}")

    if args.results_dir:
        out_dir = args.results_dir.expanduser().resolve()
        if out_dir.exists() and any(out_dir.iterdir()):
            parser.error(f"--results-dir no está vacío: {out_dir}")
    else:
        out_dir = BENCHMARKS / "results" / f"hito2-{vpu_bench.timestamp()}"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Resultados locales: {out_dir}", flush=True)

    # Los casos son independientes: cada run.py compila en su propio
    # directorio build/<opciones> y escribe en su directorio de resultados.
    results = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(run_case, case, args, out_dir): case[0]
            for case in cases
        }
        for future in as_completed(futures):
            result = future.result()
            results[result["label"]] = result
            print(
                f"{result['label']:18} "
                f"{'PASS' if result['passed'] else 'FAIL'} "
                f"({result['seconds']} s)",
                flush=True,
            )
            for error in result["errors"]:
                print(f"{'':18} {error}", flush=True)

    ordered = [results[case[0]] for case in cases]
    passed = write_summary(ordered, args, out_dir)
    failed = [result["label"] for result in ordered if not result["passed"]]
    print(f"\nResultado: {'PASS' if passed else 'FAIL'}", end="")
    if failed:
        print(f" (fallan: {', '.join(failed)})", end="")
    print(f"\nResumen: {out_dir / 'summary.md'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
