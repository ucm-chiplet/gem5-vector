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

Pasos:

1. ``make check``: compila el binario y comprueba su perfil RVV. Si falla,
   no se simula.
2. Ejecuta cada modo con ``configs/example/riscv_vpu.py``: ``offload``
   usa la VPU y ``native`` el RVV propio de MinorCPU (``--no-offload``).
3. Separa en ``stats.txt`` el primer volcado, que cubre sólo el kernel
   (m5ops de ``pathfinder.c``), del volcado final del programa.

Un modo pasa si el programa devuelve 0 y escribe ``[pathfinder] PASS``.
En offload se exige además que la VPU haya completado todos los comandos
despachados por Minor y que la red haya entregado todo lo aceptado. Con
ambos modos, sus huellas (``checksum``) deben coincidir. Los resultados se
guardan en ``results/<fecha>/``, ignorado por Git.
"""

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
CONFIG = REPOSITORY / "configs/example/riscv_vpu.py"
PASS_PREFIX = "[pathfinder] PASS:"
CHECKSUM = re.compile(r"^\[pathfinder\] checksum: (.*)$", re.M)
CPU = "system.cpu."
ENGINE = CPU + "vectorEngine."

# Métricas de la región del kernel que resume summary.md. Cada entrada es
# (etiqueta, patrón); los patrones con comodines suman todas las
# instancias (por ejemplo, todas las ALUs de todas las lanes).
SUMMARY_METRICS = (
    ("Ticks", "simTicks"),
    ("Instrucciones", "simInsts"),
    ("Ciclos CPU", CPU + "numCycles"),
    ("Comandos despachados (Minor)", CPU + "vectorOffloadCommands"),
    ("Comandos completados (VPU)", ENGINE + "commands.completed"),
    # Despachados y sin terminar: incluye los que esperan en la cola VPU.
    ("Máx. comandos en la VPU", ENGINE + "commands.peakOutstanding"),
    (
        "Ciclos con >1 comando en la VPU (Minor)",
        CPU + "vectorMultiActiveCycles",
    ),
    # Ejecución simultánea real: comandos iniciados por el secuenciador.
    ("Máx. comandos ejecutándose", ENGINE + "sequencer.peakOutstanding"),
    ("Inicios solapados", ENGINE + "sequencer.overlappedStarts"),
    ("Ciclos esperando reserva", CPU + "vectorWaitGrantCycles"),
    ("Ciclos esperando finalización", CPU + "vectorWaitCompletionCycles"),
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
)


def read_dumps(path):
    """Devuelve una lista de volcados {nombre: (valor, descripción)}."""
    dumps = []
    if not path.is_file():
        return dumps
    for line in path.read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            dumps.append({})
            continue
        match = re.match(r"^(\S+)\s+(\S+)\s+#\s*(.*)$", line)
        if match and dumps:
            name, value, description = match.groups()
            dumps[-1][name] = (value, description)
    return dumps


def metric(stats, pattern, offload=False):
    """Valor de una estadística o suma de las que encajan con el patrón.

    Devuelve None si no existe ninguna, por ejemplo las de la VPU en modo
    native, donde no se instancia. Las estadísticas vectoriales de Minor
    usan prereq y no se escriben mientras valen 0; en offload, su ausencia
    significa 0.
    """
    if "*" not in pattern:
        if pattern in stats:
            return number(stats[pattern][0])
        if offload and pattern.startswith(CPU + "vector"):
            return 0
        return None
    regex = re.compile("^" + re.escape(pattern).replace(r"\*", r"[^.]*") + "$")
    values = [number(v) for n, (v, _) in stats.items() if regex.match(n)]
    if not values:
        return None
    return sum(values)


def number(text):
    """Convierte un valor de stats.txt; nan o inf se conservan como texto."""
    try:
        value = float(text)
    except ValueError:
        return text
    if value.is_integer():
        return int(value)
    return value


def write_vector_csv(stats, path):
    """Guarda las métricas vectoriales de Minor y de la VPU, con ceros."""
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(("name", "value", "description_and_unit"))
        for name, (value, description) in sorted(stats.items()):
            if name.startswith(ENGINE) or name.startswith(CPU + "vector"):
                writer.writerow((name, value, description))


def offload_checks(stats):
    """Comprobaciones de coherencia de la VPU en la región del kernel.

    Devuelve una lista de errores; vacía si todo es coherente.
    """
    errors = []
    dispatched = metric(stats, CPU + "vectorOffloadCommands")
    completed = metric(stats, ENGINE + "commands.completed")
    if not completed:
        errors.append("la VPU no completó ningún comando")
    elif dispatched != completed:
        errors.append(
            f"comandos despachados ({dispatched}) distintos de "
            f"completados ({completed})"
        )
    accepted = metric(stats, ENGINE + "interconnect.accepted")
    delivered = metric(stats, ENGINE + "interconnect.completed")
    if accepted != delivered:
        errors.append(
            f"red: aceptados ({accepted}) distintos de entregados "
            f"({delivered})"
        )
    return errors


def run_mode(args, mode, binary, run_dir):
    """Simula un modo y devuelve su informe."""
    mode_dir = run_dir / mode
    mode_dir.mkdir()
    command = [str(args.gem5), f"--outdir={mode_dir}", str(CONFIG)]
    if mode == "native":
        command.append("--no-offload")
    else:
        command.extend(args.config_arg)
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
    checksum = CHECKSUM.search(log_text)
    dumps = read_dumps(mode_dir / "stats.txt")
    # Con la región activa hay dos volcados: kernel y programa completo.
    roi = dumps[0] if len(dumps) >= 2 else {}
    final = dumps[-1] if dumps else {}

    errors = []
    if timed_out:
        errors.append(f"timeout de {args.timeout} s")
    elif status != 0:
        errors.append(f"código de salida {status}")
    if PASS_PREFIX not in log_text:
        errors.append("no aparece el mensaje PASS")
    if not roi:
        errors.append("no hay volcado de la región del kernel")
    elif mode == "offload":
        errors.extend(offload_checks(roi))

    if roi:
        write_vector_csv(roi, mode_dir / "roi-statistics.csv")
    return {
        "command": command,
        "returncode": status,
        "timed_out": timed_out,
        "checksum": checksum.group(1) if checksum else None,
        "errors": errors,
        "passed": not errors,
        "roi_summary": {
            label: metric(roi, pattern, mode == "offload")
            for label, pattern in SUMMARY_METRICS
        },
        "final_ticks": metric(final, "simTicks"),
    }


def write_summary(report, run_dir):
    """Escribe summary.md con el resultado y las métricas del kernel."""
    runs = report["runs"]
    lines = [
        "# Pathfinder en gem5",
        "",
        f"Resultado global: **{'PASS' if report['passed'] else 'FAIL'}**.",
        "",
        f"Entrada `{report['input']}`, `NUM_RUNS={report['num_runs']}`, "
        f"commit `{report['commit']}`.",
        "",
        "| Modo | Resultado | Código | Huella | Errores |",
        "| --- | --- | --- | --- | --- |",
    ]
    for mode, run in runs.items():
        lines.append(
            f"| {mode} | {'PASS' if run['passed'] else 'FAIL'} | "
            f"{run['returncode']} | {run['checksum']} | "
            f"{'; '.join(run['errors']) or '-'} |"
        )
    if report["checksum_error"]:
        lines.extend(["", f"**{report['checksum_error']}**"])

    lines.extend(
        [
            "",
            "## Región del kernel",
            "",
            "Estadísticas entre `m5_reset_stats` y `m5_dump_stats`, que "
            "rodean sólo",
            "`runVector()`. Un guion indica una métrica inexistente en ese "
            "modo.",
            "Las peticiones lógicas de memoria son bloques de la VLSU, que "
            "se",
            "recortan por capacidad, línea o página: un comando puede "
            "generar varias.",
            "Los ticks no son una medida calibrada de rendimiento Ara.",
            "",
            "| Métrica | " + " | ".join(runs) + " |",
            "| --- |" + " --- |" * len(runs),
        ]
    )
    for label, _ in SUMMARY_METRICS:
        values = []
        for run in runs.values():
            value = run["roi_summary"][label]
            if value is None:
                values.append("-")
            else:
                values.append(str(value))
        lines.append(f"| {label} | " + " | ".join(values) + " |")

    lines.extend(
        [
            "",
            "## Archivos",
            "",
            "- `build.log`: compilación y comprobación del perfil RVV.",
            "- `<modo>/simulation.log`: salida del programa y de gem5.",
            "- `<modo>/stats.txt`: volcado del kernel y volcado final.",
            "- `<modo>/roi-statistics.csv`: métricas vectoriales de Minor y",
            "  de la VPU en la región del kernel, con ceros.",
            "- `<modo>/config.ini`, `config.json`: configuración instanciada.",
            "- `results.json`: comandos, comprobaciones y métricas.",
            "",
        ]
    )
    (run_dir / "summary.md").write_text("\n".join(lines))


def git_commit():
    """Commit actual del repositorio, con -dirty si hay cambios."""
    result = subprocess.run(
        ["git", "describe", "--always", "--dirty", "--abbrev=10"],
        cwd=REPOSITORY,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return "unknown"
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", choices=("tiny", "small"), default="tiny")
    parser.add_argument("--runs", type=int, default=1, help="NUM_RUNS")
    parser.add_argument(
        "--mode", choices=("both", "offload", "native"), default="both"
    )
    parser.add_argument(
        "--gem5", type=Path, default=REPOSITORY / "build/RISCV/gem5.opt"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        help="Maximum wall-clock seconds per simulation",
    )
    parser.add_argument(
        "--config-arg",
        action="append",
        default=[],
        help="Extra riscv_vpu.py option for offload, e.g. "
        "--config-arg=--vpu-alus-per-lane=2 (repeatable)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        help="Empty or new directory for this run; default: "
        "results/<UTC date>",
    )
    args = parser.parse_args()
    args.gem5 = args.gem5.expanduser().resolve()
    if not args.gem5.is_file():
        parser.error(f"gem5 no existe: {args.gem5}")
    if args.timeout <= 0 or args.runs <= 0:
        parser.error("--timeout y --runs deben ser positivos")

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
        f"INPUT={args.input}",
        f"NUM_RUNS={args.runs}",
        "check",
    ]
    with (run_dir / "build.log").open("w") as log:
        build_status = subprocess.run(
            build_command, stdout=log, stderr=subprocess.STDOUT
        ).returncode
    binary = BENCHMARK / f"build/{args.input}-runs{args.runs}/pathfinder"

    report = {
        "benchmark": "pathfinder",
        "input": args.input,
        "num_runs": args.runs,
        "commit": git_commit(),
        "config_args": args.config_arg,
        "build_command": build_command,
        "build_returncode": build_status,
        "checksum_error": None,
        "runs": {},
    }
    modes = ("offload", "native") if args.mode == "both" else (args.mode,)
    if build_status == 0:
        for mode in modes:
            run = run_mode(args, mode, binary, run_dir)
            report["runs"][mode] = run
            print(f"{mode}: {'PASS' if run['passed'] else 'FAIL'}", flush=True)
    else:
        print("Compilación o perfil RVV fallidos: ver build.log", flush=True)

    checksums = {run["checksum"] for run in report["runs"].values()}
    if len(report["runs"]) > 1 and len(checksums) != 1:
        report["checksum_error"] = "Las huellas de los modos no coinciden."
    report["passed"] = (
        build_status == 0
        and report["checksum_error"] is None
        and all(
            report["runs"].get(mode, {}).get("passed", False) for mode in modes
        )
    )
    (run_dir / "results.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    )
    write_summary(report, run_dir)
    print(f"Resumen: {run_dir / 'summary.md'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
