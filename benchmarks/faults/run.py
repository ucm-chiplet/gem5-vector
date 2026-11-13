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

"""Fault de página a mitad de un acceso unit-stride (criterio 8 del hito 2).

Compila el programa con make check (VARIANT=load o store) y lo simula en
gem5 con la traza MinorExecute. Un vle32.v o vse32.v con vl = 8 empieza
INSIDE elementos antes del final de una página mapeada con mmap; el
elemento INSIDE cae en la página siguiente, sin mapear.

En modo SE gem5 no tiene manejador de traps: el fault termina la
simulación con un panic. El resultado esperado, y por tanto PASS, es:

- el programa llega al acceso ("[faults] armed") y no continúa
  ("UNEXPECTED" no aparece);
- gem5 termina con el panic de fault de página en PAGE_BASE + 4096;
- en offload, Minor traza el fault de la VPU con esa dirección,
  element=INSIDE y vstart=INSIDE, el valor de vstart con el que se
  invoca el fault.

En native (RVV de Minor) se comprueban los dos primeros puntos: es el
contraste del mismo programa sin VPU. Resultados en results/<fecha UTC>/.

Uso:
  python3 benchmarks/faults/run.py                       # load y store
  python3 benchmarks/faults/run.py --variant store --inside 1 -v
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))

import vpu_bench  # noqa: E402

DIRECTORY = Path(__file__).resolve().parent
PAGE_BASE = 0x40000000
PAGE_SIZE = 4096
VL = 8
FAULT_TRACE = re.compile(
    r"VPU Offload returned a memory fault: .* vaddr=(0x[0-9a-f]+) "
    r"element=(-?\d+) vstart=(-?\d+)"
)


def run_variant(args, variant, mode, run_dir):
    """Compila y simula una variante en un modo; devuelve su informe."""
    out = run_dir / f"{variant}-{mode}"
    out.mkdir()
    build = [
        "make",
        "-C",
        str(DIRECTORY),
        f"VARIANT={variant}",
        f"INSIDE={args.inside}",
        "check",
    ]
    build_status, _ = vpu_bench.run_logged(
        build, out / "build.log", verbose=args.verbose
    )
    report = {"build_command": build, "build_returncode": build_status}
    if build_status != 0:
        report.update(passed=False, errors=["compilación o perfil RVV"])
        return report

    binary = DIRECTORY / f"build/{variant}-i{args.inside}/faults"
    command = [str(args.gem5), f"--outdir={out}"]
    if mode == "offload":
        command += ["--debug-flags=MinorExecute", "--debug-file=trace.out"]
    command.append(str(vpu_bench.CONFIG))
    if mode == "native":
        command.append("--no-offload")
    command.append(str(binary))
    status, timed_out = vpu_bench.run_logged(
        command,
        out / "simulation.log",
        verbose=args.verbose,
        timeout=args.timeout,
        cwd=vpu_bench.REPOSITORY,
    )
    log = (out / "simulation.log").read_text()
    address = PAGE_BASE + PAGE_SIZE
    errors = []
    if timed_out:
        errors.append(f"timeout de {args.timeout} s")
    if "[faults] armed" not in log:
        errors.append("el programa no llegó al acceso")
    if "UNEXPECTED" in log:
        errors.append("el acceso no produjo fault")
    if status == 0:
        errors.append("gem5 terminó sin el panic del fault")
    expected_panic = (
        f"Page table fault when accessing virtual address {address:#x}"
    )
    if expected_panic not in log:
        errors.append(f"no aparece: {expected_panic}")
    traced = None
    if mode == "offload":
        trace = out / "trace.out"
        text = trace.read_text() if trace.is_file() else ""
        match = FAULT_TRACE.search(text)
        if not match:
            errors.append("Minor no trazó un fault de memoria de la VPU")
        else:
            traced = {
                "vaddr": match.group(1),
                "element": int(match.group(2)),
                "vstart": int(match.group(3)),
            }
            expected = {
                "vaddr": f"{address:#x}",
                "element": args.inside,
                "vstart": args.inside,
            }
            if traced != expected:
                errors.append(f"traza {traced}, esperado {expected}")
            # La traza ocupa mucho y ya se ha comprobado: sólo se guarda
            # la línea del fault.
            (out / "fault-trace.txt").write_text(match.group(0) + "\n")
            trace.unlink()
    report.update(
        command=command,
        returncode=status,
        traced_fault=traced,
        passed=not errors,
        errors=errors,
    )
    return report


def write_summary(report, run_dir):
    lines = [
        "# Fault de página a mitad de un acceso unit-stride",
        "",
        f"- Commit: `{report['commit']}`",
        f"- vl = {VL}, primer elemento sin mapear: {report['inside']}",
        "",
        "| Caso | Resultado | Fault trazado | Errores |",
        "| --- | --- | --- | --- |",
    ]
    for name, run in report["runs"].items():
        traced = run.get("traced_fault") or "—"
        errors = "; ".join(run["errors"]) or "—"
        result = "PASS" if run["passed"] else "FAIL"
        lines.append(f"| {name} | {result} | {traced} | {errors} |")
    (run_dir / "summary.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--variant", choices=("both", "load", "store"), default="both"
    )
    parser.add_argument(
        "--inside",
        type=int,
        default=4,
        help=f"Mapped elements before the fault, 1..{VL - 1} (default: 4)",
    )
    parser.add_argument(
        "--mode", choices=("both", "offload", "native"), default="both"
    )
    vpu_bench.add_common_arguments(parser)
    parser.add_argument("--results-dir", type=Path)
    args = parser.parse_args()
    if not 1 <= args.inside < VL:
        parser.error(f"--inside debe estar entre 1 y {VL - 1}")
    args.gem5 = args.gem5.expanduser().resolve()
    if not args.gem5.is_file():
        parser.error(f"gem5 no existe: {args.gem5}")

    if args.results_dir:
        run_dir = args.results_dir.expanduser().resolve()
    else:
        run_dir = DIRECTORY / "results" / vpu_bench.timestamp()
    run_dir.mkdir(parents=True, exist_ok=True)
    print(f"Resultados locales: {run_dir}", flush=True)

    variants = ("load", "store") if args.variant == "both" else (args.variant,)
    modes = ("offload", "native") if args.mode == "both" else (args.mode,)
    report = {
        "benchmark": "faults",
        "commit": vpu_bench.git_commit(),
        "inside": args.inside,
        "build_returncode": 0,
        "runs": {},
    }
    for variant in variants:
        for mode in modes:
            run = run_variant(args, variant, mode, run_dir)
            report["runs"][f"{variant}-{mode}"] = run
            if run["build_returncode"] != 0:
                report["build_returncode"] = run["build_returncode"]
            print(f"{variant}-{mode}: {'PASS' if run['passed'] else 'FAIL'}")
            for error in run["errors"]:
                print(f"  {error}")
    report["passed"] = all(run["passed"] for run in report["runs"].values())
    (run_dir / "results.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    )
    write_summary(report, run_dir)
    print(f"Resultado: {'PASS' if report['passed'] else 'FAIL'}")
    print(f"Resumen: {run_dir / 'summary.md'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
