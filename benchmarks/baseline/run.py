# SPDX-License-Identifier: BSD-3-Clause

"""Baseline del hito 1: compila y ejecuta un programa en gem5.

Compila con make (GCC de la toolchain RISC-V), simula offload (MinorCPU
con VPU) y native (RVV de MinorCPU) y exige en cada modo código 0 y el
mensaje PASS del programa. Las estadísticas abarcan todo el programa.
Resultados en results/<fecha UTC>/ (o en --results-dir), ignorado por Git.

Uso:
  python3 benchmarks/baseline/run.py                       # 21 casos
  python3 benchmarks/baseline/run.py --program baseline-simple
  python3 benchmarks/baseline/run.py --mode offload -v     # salida detallada
"""

import argparse
import csv
import json
import re
import sys
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path

BENCHMARK = Path(__file__).resolve().parent
REPOSITORY = BENCHMARK.parents[1]
sys.path.insert(0, str(BENCHMARK.parent / "common"))

# run_logged guarda la salida en el registro y, con --verbose, la muestra.
from vpu_bench import run_logged  # noqa: E402

PROGRAMS = {
    "baseline": {
        "pass_message": "[SUITE baseline] PASS: 21 baseline cases",
        "description": (
            "Cada modo verifica los 21 casos contra la referencia escalar."
        ),
    },
    "baseline-simple": {
        "pass_message": "[baseline-simple] PASS: resultado={16,27,38,49}",
        "description": (
            "Cada modo verifica A + B + 5: resultado={16,27,38,49}."
        ),
    },
}


def read_stats(path):
    """Read the final gem5 statistics dump with names and descriptions."""
    values = {}
    if not path.is_file():
        return values
    for line in path.read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            values = {}
        match = re.match(r"^(\S+)\s+(\S+)\s+#\s*(.*)$", line)
        if match:
            name, value, description = match.groups()
            values[name] = {"value": value, "description": description}
    return values


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--program",
        choices=tuple(PROGRAMS),
        default="baseline",
        help="Program to build and run",
    )
    parser.add_argument(
        "--gem5",
        type=Path,
        default=REPOSITORY / "build/RISCV/gem5.opt",
        help="gem5 binary (default: build/RISCV/gem5.opt)",
    )
    parser.add_argument("--cc", help="Override the RISC-V C compiler")
    parser.add_argument(
        "--mode",
        choices=("both", "offload", "native"),
        default="both",
        help="offload: MinorCPU + VPU; native: MinorCPU RVV only; "
        "both (default)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Maximum wall-clock seconds per simulation (default: 120)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Also print make, gem5 and program output while running",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        help="Empty or new directory for this run; default: "
        "results/<UTC date>",
    )
    args = parser.parse_args()
    program = PROGRAMS[args.program]
    binary = BENCHMARK / "build" / args.program
    gem5 = args.gem5.expanduser().resolve()
    if not gem5.is_file():
        parser.error(f"gem5 no existe: {gem5}")
    if args.timeout <= 0:
        parser.error("--timeout debe ser positivo")

    if args.results_dir:
        run_dir = args.results_dir.expanduser().resolve()
        if run_dir.exists() and any(run_dir.iterdir()):
            parser.error(f"--results-dir no está vacío: {run_dir}")
    else:
        run_name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        run_dir = BENCHMARK / "results" / run_name
    run_dir.mkdir(parents=True, exist_ok=True)
    print(f"Resultados locales: {run_dir}", flush=True)
    build_command = [
        "make",
        "-C",
        str(BENCHMARK),
        "-B",
        f"build/{args.program}",
    ]
    if args.cc:
        build_command.append(f"CC={args.cc}")
    if args.verbose:
        print(f"\n=== {args.program}: compilación ===", flush=True)
    build_status, _ = run_logged(
        build_command, run_dir / "build.log", verbose=args.verbose
    )
    report = {
        "benchmark": args.program,
        "pass_message": program["pass_message"],
        "scope": "functional; entire program, including scalar checks/logs",
        "build_command": build_command,
        "build_returncode": build_status,
        "runs": {},
    }
    modes = ("offload", "native") if args.mode == "both" else (args.mode,)
    if build_status == 0:
        for mode in modes:
            mode_dir = run_dir / mode
            mode_dir.mkdir()
            command = [
                str(gem5),
                f"--outdir={mode_dir}",
                str(REPOSITORY / "configs/example/riscv_vpu.py"),
            ]
            if mode == "native":
                command.append("--no-offload")
            command.append(str(binary))
            if args.verbose:
                print(f"\n=== {args.program}: modo {mode} ===", flush=True)
                print(" ".join(command), flush=True)
            status, timed_out = run_logged(
                command,
                mode_dir / "simulation.log",
                verbose=args.verbose,
                timeout=args.timeout,
                cwd=REPOSITORY,
            )
            log_text = (mode_dir / "simulation.log").read_text()
            passed = status == 0 and program["pass_message"] in log_text
            stats = read_stats(mode_dir / "stats.txt")
            with (mode_dir / "vpu-statistics.csv").open("w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(("name", "value", "description_and_unit"))
                for name, entry in sorted(stats.items()):
                    if ".vectorEngine." in name or ".vectorInterface." in name:
                        writer.writerow(
                            (name, entry["value"], entry["description"])
                        )
            report["runs"][mode] = {
                "command": command,
                "returncode": status,
                "timed_out": timed_out,
                "passed": passed,
                "stats": stats,
            }
            print(f"{mode}: {'PASS' if passed else 'FAIL'}", flush=True)

    passed = build_status == 0 and all(
        report["runs"].get(mode, {}).get("passed", False) for mode in modes
    )
    report["passed"] = passed
    (run_dir / "results.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    )
    summary = [
        f"# Resultado del benchmark {args.program}",
        "",
        f"Resultado global: **{'PASS' if passed else 'FAIL'}**.",
        "",
        program["description"],
        "Las estadísticas abarcan todo el programa, incluidos los logs;",
        "los ticks no representan una medida calibrada de rendimiento Ara.",
        "",
        "| Modo | Resultado | Código de salida | Timeout |",
        "| --- | --- | --- | --- |",
    ]
    for mode, run in report["runs"].items():
        summary.append(
            f"| {mode} | {'PASS' if run['passed'] else 'FAIL'} | "
            f"{run['returncode']} | {run['timed_out']} |"
        )
    summary.extend(
        [
            "",
            "- `build.log`: compilación del benchmark.",
            "- `offload/`: MinorCPU con VPU; "
            "`native/`: RVV nativo de MinorCPU.",
            "- `simulation.log`: salida del programa y diagnósticos de gem5.",
            "- `config.json` / `config.ini`: "
            "configuración realmente instanciada.",
            "- `stats.txt`: todas las estadísticas con descripción y unidad.",
            "- `vpu-statistics.csv`: "
            "métricas de VPU e interfaz CPU, con ceros.",
            "- `results.json`: comandos, códigos de salida y estadísticas.",
            "",
            "Un CSV vacío en modo native es normal: no se instancia la VPU.",
            "Si falla la compilación, consulta `build.log`.",
            "",
        ]
    )
    (run_dir / "summary.md").write_text("\n".join(summary))
    print(f"Resultado: {'PASS' if passed else 'FAIL'}")
    print(f"Resumen: {run_dir / 'summary.md'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
