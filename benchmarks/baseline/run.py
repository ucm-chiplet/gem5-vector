# SPDX-License-Identifier: BSD-3-Clause

"""Build and run a functional baseline program; keep artifacts local."""

import argparse
import csv
import json
import re
import subprocess
import sys
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path

BENCHMARK = Path(__file__).resolve().parent
REPOSITORY = BENCHMARK.parents[1]
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--program",
        choices=tuple(PROGRAMS),
        default="baseline",
        help="Program to build and run",
    )
    parser.add_argument(
        "--gem5", type=Path, default=REPOSITORY / "build/RISCV/gem5.opt"
    )
    parser.add_argument("--cc", help="Override the RISC-V C compiler")
    parser.add_argument(
        "--mode", choices=("both", "offload", "native"), default="both"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Maximum wall-clock seconds per simulation",
    )
    args = parser.parse_args()
    program = PROGRAMS[args.program]
    binary = BENCHMARK / "build" / args.program
    gem5 = args.gem5.expanduser().resolve()
    if not gem5.is_file():
        parser.error(f"gem5 no existe: {gem5}")
    if args.timeout <= 0:
        parser.error("--timeout debe ser positivo")

    run_name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run_dir = BENCHMARK / "results" / run_name
    run_dir.mkdir(parents=True)
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
    with (run_dir / "build.log").open("w") as log:
        build_status = subprocess.run(
            build_command, stdout=log, stderr=subprocess.STDOUT
        ).returncode
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
            timed_out = False
            with (mode_dir / "simulation.log").open("w") as log:
                try:
                    status = subprocess.run(
                        command,
                        cwd=REPOSITORY,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        timeout=args.timeout,
                    ).returncode
                except subprocess.TimeoutExpired:
                    status = None
                    timed_out = True
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
    print(f"Resumen: {run_dir / 'summary.md'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
