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

"""Runner y barrido comunes para los benchmarks de la VPU.

Cada benchmark describe en un ``Benchmark`` cómo se compila (opciones que
pasan a variables de ``make``), dónde queda su binario y qué métricas
resume. Sus ``run.py`` y ``sweep.py`` sólo llaman a ``run_main`` y
``sweep_main`` con esa descripción.

Contrato del programa: imprime ``[<nombre>] checksum: ...`` y
``[<nombre>] PASS: ...``, devuelve 0 si su verificación pasa y rodea el
kernel con ``m5_reset_stats`` y ``m5_dump_stats``, de modo que el primer
volcado de ``stats.txt`` es la región del kernel. ``make check`` compila el
binario y comprueba su perfil RVV.

``run_main``:

1. Ejecuta ``make check``; si falla, no simula.
2. Simula los modos pedidos con ``configs/example/riscv_vpu.py``:
   ``offload`` usa la VPU y ``native`` el RVV de MinorCPU.
3. Un modo pasa si el programa devuelve 0, escribe PASS y hay volcado del
   kernel. En offload se exige además que la VPU complete todo lo que
   despacha Minor y que la red entregue todo lo aceptado. Con dos modos,
   las huellas deben coincidir.

``sweep_main`` ejecuta ``run.py --mode=offload`` para una lista de
configuraciones y compara cada una con la primera.

Con ``--verbose`` (``-v``) la salida de ``make``, de gem5 y del programa
se muestra también en el terminal mientras se ejecuta, y al final se
imprimen las métricas de la región del kernel. Sin ella sólo se imprime el
resultado de cada modo; los registros completos se guardan siempre.

Los resultados quedan en ``<benchmark>/results/``, ignorado por Git.
"""

import argparse
import csv
import json
import re
import subprocess
import sys
import threading
from dataclasses import (
    dataclass,
    field,
)
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path
from typing import (
    Callable,
    Optional,
    Tuple,
)

REPOSITORY = Path(__file__).resolve().parents[2]
CONFIG = REPOSITORY / "configs/example/riscv_vpu.py"
DEFAULT_GEM5 = REPOSITORY / "build/RISCV/gem5.opt"
CPU = "system.cpu."
ENGINE = CPU + "vectorEngine."

# Métricas comunes a todos los benchmarks, antes de las propias.
COMMON_SUMMARY_METRICS = (
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
    (
        "Ticks con >1 comando ejecutándose",
        ENGINE + "sequencer.multiActiveTicks",
    ),
    ("Ciclos esperando reserva", CPU + "vectorWaitGrantCycles"),
    (
        "Ciclos con comandos en la VPU (Minor)",
        CPU + "vectorWaitCompletionCycles",
    ),
)

# Métricas comunes del barrido: ocupación por recurso. Se solapan entre sí
# y no suman el tiempo total.
COMMON_SWEEP_METRICS = (
    ("Ticks", "simTicks"),
    ("Máx. en VPU", ENGINE + "commands.peakOutstanding"),
    ("Máx. ejecutándose", ENGINE + "sequencer.peakOutstanding"),
    ("Inicios solapados", ENGINE + "sequencer.overlappedStarts"),
    ("Ticks >1 ejecutándose", ENGINE + "sequencer.multiActiveTicks"),
    ("VLSU ocupada", ENGINE + "vlsu.busyTicks"),
    ("ALUs ocupadas", ENGINE + "lane*.alu*.busyTicks"),
    ("Espera VRF", ENGINE + "lrf*.grantWaitTicks"),
)


@dataclass(frozen=True)
class BuildOption:
    """Opción de línea de comandos que pasa a una variable de make.

    ``name`` es el nombre de la opción sin guiones (``input`` da
    ``--input``) y ``make_var`` la variable que recibe su valor.
    """

    name: str
    make_var: str
    default: object
    help: str
    choices: Optional[Tuple] = None
    type: Callable = str
    positive: bool = False


@dataclass(frozen=True)
class Benchmark:
    """Descripción de un benchmark para el runner y el barrido."""

    name: str
    directory: Path
    title: str
    # Qué rodean las m5ops, para el resumen.
    roi_description: str
    options: Tuple[BuildOption, ...]
    # Directorio de compilación, relativo al benchmark, para unas opciones.
    build_dir: Callable[[argparse.Namespace], str]
    binary_name: str
    summary_metrics: Tuple = ()
    sweep_metrics: Tuple = ()
    default_sweep: Tuple = ()
    summary_notes: Tuple[str, ...] = ()
    # Comprobaciones adicionales de offload: devuelven errores.
    offload_checks: Tuple[Callable, ...] = field(default_factory=tuple)
    # Opciones de riscv_vpu.py que el benchmark necesita siempre en offload,
    # por ejemplo para habilitar SEW o LMUL fuera del perfil por defecto.
    # Van antes de las de --config-arg, que pueden sustituirlas.
    config_args: Tuple[str, ...] = ()

    @property
    def pass_prefix(self):
        return f"[{self.name}] PASS:"

    @property
    def checksum_regex(self):
        return re.compile(
            r"^\[" + re.escape(self.name) + r"\] checksum: (.*)$", re.M
        )


def run_logged(
    command, log_path, verbose=False, timeout=None, cwd=None, prefix=""
):
    """Ejecuta command guardando su salida en log_path.

    stdout y stderr se mezclan en el registro. Con verbose, cada línea se
    imprime además en el terminal en cuanto llega, precedida de prefix.
    Devuelve (código de salida, timeout): el código es None si se superó
    timeout segundos, en cuyo caso el proceso se termina.
    """
    with log_path.open("w") as log:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            bufsize=1,
        )

        # Un hilo copia la salida para que wait() pueda aplicar el timeout
        # aunque el proceso no escriba nada durante un rato.
        def copy_output():
            for line in process.stdout:
                log.write(line)
                if verbose:
                    print(prefix + line, end="", flush=True)

        reader = threading.Thread(target=copy_output)
        reader.start()
        try:
            status = process.wait(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            status = None
            timed_out = True
        reader.join()
    return status, timed_out


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


def number(text):
    """Convierte un valor de stats.txt; nan o inf se conservan como texto."""
    try:
        value = float(text)
    except ValueError:
        return text
    if value.is_integer():
        return int(value)
    return value


def metric(stats, pattern, offload=False):
    """Valor de una estadística o suma de las que encajan con el patrón.

    Devuelve None si no existe ninguna, por ejemplo las de la VPU en modo
    native, donde no se instancia. Versiones anteriores de Minor ocultaban
    sus métricas vectoriales mientras valían 0; en offload, su ausencia se
    sigue tratando como 0 para poder leer resultados antiguos.
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


def write_vector_csv(stats, path):
    """Guarda las métricas vectoriales de Minor y de la VPU, con ceros."""
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(("name", "value", "description_and_unit"))
        for name, (value, description) in sorted(stats.items()):
            if name.startswith(ENGINE) or name.startswith(CPU + "vector"):
                writer.writerow((name, value, description))


def common_offload_checks(stats):
    """Coherencia de la VPU en la región del kernel; devuelve errores."""
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


def require_nonzero(*patterns):
    """Comprobación de offload: cada estadística debe existir y no ser 0.

    Sirve para exigir que la VPU haya ejecutado de verdad el trabajo del
    benchmark en la unidad esperada (ALU, FPU, SlideUnit, VLSU...).
    """

    def check(stats):
        errors = []
        for pattern in patterns:
            value = metric(stats, ENGINE + pattern)
            if not value:
                errors.append(
                    f"{pattern} vale {value}: la VPU no hizo ese " "trabajo"
                )
        return errors

    return check


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


def timestamp():
    """Nombre de directorio con la fecha UTC actual."""
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")


def add_build_options(parser, bench):
    """Añade al parser las opciones de compilación del benchmark."""
    for option in bench.options:
        parser.add_argument(
            f"--{option.name.replace('_', '-')}",
            dest=option.name,
            type=option.type,
            choices=option.choices,
            default=option.default,
            help=option.help,
        )


def check_build_options(parser, bench, args):
    """Valida las opciones que deben ser positivas."""
    for option in bench.options:
        if option.positive and getattr(args, option.name) <= 0:
            parser.error(
                f"--{option.name.replace('_', '-')} debe ser positivo"
            )


def build_values(bench, args):
    """Valores de compilación {nombre: valor} de una ejecución."""
    return {
        option.name: getattr(args, option.name) for option in bench.options
    }


def run_mode(bench, args, mode, binary, run_dir):
    """Simula un modo y devuelve su informe."""
    mode_dir = run_dir / mode
    mode_dir.mkdir()
    command = [str(args.gem5), f"--outdir={mode_dir}", str(CONFIG)]
    if mode == "native":
        command.append("--no-offload")
    else:
        command.extend(bench.config_args)
        command.extend(args.config_arg)
    command.append(str(binary))

    if args.verbose:
        print(f"\n=== {bench.name}: modo {mode} ===", flush=True)
        print(" ".join(command), flush=True)
    status, timed_out = run_logged(
        command,
        mode_dir / "simulation.log",
        verbose=args.verbose,
        timeout=args.timeout,
        cwd=REPOSITORY,
    )

    log_text = (mode_dir / "simulation.log").read_text()
    checksum = bench.checksum_regex.search(log_text)
    dumps = read_dumps(mode_dir / "stats.txt")
    # Con la región activa hay dos volcados: kernel y programa completo.
    roi = dumps[0] if len(dumps) >= 2 else {}
    final = dumps[-1] if dumps else {}

    errors = []
    if timed_out:
        errors.append(f"timeout de {args.timeout} s")
    elif status != 0:
        errors.append(f"código de salida {status}")
    if bench.pass_prefix not in log_text:
        errors.append("no aparece el mensaje PASS")
    if not roi:
        errors.append("no hay volcado de la región del kernel")
    elif mode == "offload":
        errors.extend(common_offload_checks(roi))
        for check in bench.offload_checks:
            errors.extend(check(roi))

    if roi:
        write_vector_csv(roi, mode_dir / "roi-statistics.csv")
    metrics = COMMON_SUMMARY_METRICS + bench.summary_metrics
    return {
        "command": command,
        "returncode": status,
        "timed_out": timed_out,
        "checksum": checksum.group(1) if checksum else None,
        "errors": errors,
        "passed": not errors,
        "roi_summary": {
            label: metric(roi, pattern, mode == "offload")
            for label, pattern in metrics
        },
        "final_ticks": metric(final, "simTicks"),
    }


def format_options(values):
    """Opciones de compilación como texto para los resúmenes."""
    return ", ".join(f"`{name}={value}`" for name, value in values.items())


def write_run_summary(bench, report, run_dir):
    """Escribe summary.md con el resultado y las métricas del kernel."""
    runs = report["runs"]
    lines = [
        f"# {bench.title} en gem5",
        "",
        f"Resultado global: **{'PASS' if report['passed'] else 'FAIL'}**.",
        "",
        f"{format_options(report['build'])}, commit `{report['commit']}`.",
    ]
    config_args = report["fixed_config_args"] + report["config_args"]
    if config_args:
        lines.append("Opciones de la VPU: `" + " ".join(config_args) + "`.")
    lines.extend(
        [
            "",
            "| Modo | Resultado | Código | Huella | Errores |",
            "| --- | --- | --- | --- | --- |",
        ]
    )
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
            f"rodean sólo {bench.roi_description}.",
            "Un guion indica una métrica inexistente en ese modo.",
            "Los ticks no son una medida calibrada de rendimiento Ara.",
            *bench.summary_notes,
            "",
            "| Métrica | " + " | ".join(runs) + " |",
            "| --- |" + " --- |" * len(runs),
        ]
    )
    for label, _ in COMMON_SUMMARY_METRICS + bench.summary_metrics:
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


def print_report(bench, report):
    """Imprime en el terminal errores y métricas de la región del kernel."""
    runs = report["runs"]
    if not runs:
        return
    print(f"\n=== {bench.name}: región del kernel ===")
    labels = [label for label, _ in COMMON_SUMMARY_METRICS]
    labels += [label for label, _ in bench.summary_metrics]
    width = max(len(label) for label in labels)
    print(f"{'':{width}}  " + "  ".join(f"{mode:>14}" for mode in runs))
    for label in labels:
        values = []
        for run in runs.values():
            value = run["roi_summary"][label]
            values.append(f"{'-' if value is None else value:>14}")
        print(f"{label:{width}}  " + "  ".join(values))
    for mode, run in runs.items():
        print(f"{mode}: huella {run['checksum']}")


def add_common_arguments(parser):
    """Opciones de ejecución comunes a run.py y sweep.py."""
    parser.add_argument(
        "--gem5",
        type=Path,
        default=DEFAULT_GEM5,
        help="gem5 binary (default: build/RISCV/gem5.opt)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        help="Maximum wall-clock seconds per simulation (default: 600)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Also print make, gem5 and program output while running, "
        "and the kernel metrics at the end",
    )


def run_main(bench, description):
    """Punto de entrada de ``run.py`` de un benchmark."""
    parser = argparse.ArgumentParser(
        description=description,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_build_options(parser, bench)
    parser.add_argument(
        "--mode",
        choices=("both", "offload", "native"),
        default="both",
        help="offload: MinorCPU + VPU; native: MinorCPU RVV only; "
        "both (default): run both and compare their checksums",
    )
    add_common_arguments(parser)
    parser.add_argument(
        "--config-arg",
        action="append",
        default=[],
        help="Extra riscv_vpu.py option for offload, e.g. "
        "--config-arg=--vpu-set=lane.alus_per_lane=2 (repeatable)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        help="Empty or new directory for this run; default: "
        "results/<UTC date>",
    )
    args = parser.parse_args()
    check_build_options(parser, bench, args)
    args.gem5 = args.gem5.expanduser().resolve()
    if not args.gem5.is_file():
        parser.error(f"gem5 no existe: {args.gem5}")
    if args.timeout <= 0:
        parser.error("--timeout debe ser positivo")

    if args.results_dir:
        run_dir = args.results_dir.expanduser().resolve()
        if run_dir.exists() and any(run_dir.iterdir()):
            parser.error(f"--results-dir no está vacío: {run_dir}")
    else:
        run_dir = bench.directory / "results" / timestamp()
    run_dir.mkdir(parents=True, exist_ok=True)
    print(f"Resultados locales: {run_dir}", flush=True)

    build = build_values(bench, args)
    build_command = ["make", "-C", str(bench.directory)]
    build_command.extend(
        f"{option.make_var}={build[option.name]}" for option in bench.options
    )
    build_command.append("check")
    if args.verbose:
        print(f"\n=== {bench.name}: compilación ===", flush=True)
        print(" ".join(build_command), flush=True)
    build_status, _ = run_logged(
        build_command, run_dir / "build.log", verbose=args.verbose
    )
    binary = bench.directory / bench.build_dir(args) / bench.binary_name

    report = {
        "benchmark": bench.name,
        "build": build,
        "commit": git_commit(),
        "fixed_config_args": list(bench.config_args),
        "config_args": args.config_arg,
        "build_command": build_command,
        "build_returncode": build_status,
        "checksum_error": None,
        "runs": {},
    }
    modes = ("offload", "native") if args.mode == "both" else (args.mode,)
    if build_status == 0:
        for mode in modes:
            run = run_mode(bench, args, mode, binary, run_dir)
            report["runs"][mode] = run
            print(f"{mode}: {'PASS' if run['passed'] else 'FAIL'}", flush=True)
            for error in run["errors"]:
                print(f"  {error}", flush=True)
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
    write_run_summary(bench, report, run_dir)
    if args.verbose:
        print_report(bench, report)
    if report["checksum_error"]:
        print(report["checksum_error"])
    print(f"Resultado: {'PASS' if report['passed'] else 'FAIL'}")
    print(f"Resumen: {run_dir / 'summary.md'}")
    return 0 if report["passed"] else 1


def run_sweep_config(bench, args, config, sweep_dir):
    """Ejecuta una configuración del barrido y devuelve su resumen."""
    out_dir = sweep_dir / config["label"]
    command = [
        sys.executable,
        str(bench.directory / "run.py"),
        "--mode=offload",
        f"--timeout={args.timeout}",
        f"--gem5={args.gem5}",
        f"--results-dir={out_dir}",
    ]
    if args.verbose:
        command.append("--verbose")
    for option in bench.options:
        flag = option.name.replace("_", "-")
        command.append(f"--{flag}={getattr(args, option.name)}")
    command.extend(f"--config-arg={item}" for item in config["args"])
    # run.py guarda sus registros en out_dir; aquí sólo se muestra su salida
    # en el terminal si se pidió --verbose.
    if args.verbose:
        status = subprocess.run(command).returncode
    else:
        status = subprocess.run(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT
        ).returncode

    result = {
        "label": config["label"],
        "args": config["args"],
        "returncode": status,
        "passed": False,
        "checksum": None,
        "errors": [],
        "metrics": {},
    }
    report_path = out_dir / "results.json"
    if not report_path.is_file():
        result["errors"].append("run.py no produjo results.json")
        return result
    report = json.loads(report_path.read_text())
    offload = report["runs"].get("offload")
    if offload is None:
        result["errors"].append("compilación o perfil RVV fallidos")
        return result
    result["passed"] = offload["passed"]
    result["checksum"] = offload["checksum"]
    result["errors"] = offload["errors"]
    dumps = read_dumps(out_dir / "offload/stats.txt")
    roi = dumps[0] if len(dumps) >= 2 else {}
    result["metrics"] = {
        label: metric(roi, pattern, offload=True)
        for label, pattern in COMMON_SWEEP_METRICS + bench.sweep_metrics
    }
    return result


def relative(value, reference):
    """Variación porcentual respecto a la referencia, como texto."""
    if not isinstance(value, (int, float)) or not reference:
        return "-"
    change = 100.0 * (value - reference) / reference
    return f"{change:+.1f} %"


def write_sweep_summary(bench, sweep, sweep_dir):
    """Escribe summary.md con la tabla comparativa del barrido."""
    results = sweep["results"]
    base = results[0]
    base_ticks = base["metrics"].get("Ticks")
    columns = [
        label for label, _ in COMMON_SWEEP_METRICS + bench.sweep_metrics
    ]
    columns = columns[1:]
    lines = [
        f"# Barrido de configuraciones con {bench.title}",
        "",
        f"Resultado global: **{'PASS' if sweep['passed'] else 'FAIL'}**.",
        "",
        f"{format_options(sweep['build'])}, commit `{sweep['commit']}`, "
        f"modo offload. Referencia: `{base['label']}`.",
        "",
        "Métricas de la región del kernel, en ticks salvo recuentos. Los",
        "tiempos de ocupación de cada recurso se solapan y no suman el total.",
        "",
        "| Configuración | Resultado | Huella igual | Ticks | Δ ticks | "
        + " | ".join(columns)
        + " |",
        "| --- | --- | --- | --- | --- |" + " --- |" * len(columns),
    ]
    for result in results:
        metrics = result["metrics"]
        same = result["checksum"] == base["checksum"]
        values = []
        for label in columns:
            value = metrics.get(label)
            if value is None:
                values.append("-")
            else:
                values.append(str(value))
        lines.append(
            f"| `{result['label']}` | "
            f"{'PASS' if result['passed'] else 'FAIL'} | "
            f"{'sí' if same else 'NO'} | {metrics.get('Ticks', '-')} | "
            f"{relative(metrics.get('Ticks'), base_ticks)} | "
            + " | ".join(values)
            + " |"
        )
    lines.extend(["", "## Opciones de cada configuración", ""])
    for result in results:
        options = " ".join(result["args"]) or "(valores por defecto)"
        errors = "; ".join(result["errors"])
        line = f"- `{result['label']}`: `{options}`"
        if errors:
            line += f". Errores: {errors}"
        lines.append(line)
    lines.append("")
    (sweep_dir / "summary.md").write_text("\n".join(lines))


def sweep_main(bench, description):
    """Punto de entrada de ``sweep.py`` de un benchmark."""
    parser = argparse.ArgumentParser(
        description=description,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_build_options(parser, bench)
    add_common_arguments(parser)
    parser.add_argument(
        "--configs",
        type=Path,
        help='JSON list of {"label": ..., "args": [riscv_vpu.py options]}; '
        "the first one is the reference",
    )
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        help="Run only these labels; the first config is always kept",
    )
    args = parser.parse_args()
    check_build_options(parser, bench, args)
    args.gem5 = args.gem5.expanduser().resolve()

    configs = list(bench.default_sweep)
    if args.configs:
        configs = json.loads(args.configs.read_text())
    labels = [config["label"] for config in configs]
    if not configs or len(set(labels)) != len(labels):
        parser.error("la lista necesita etiquetas únicas y no vacía")
    if args.only:
        configs = [configs[0]] + [
            config for config in configs[1:] if config["label"] in args.only
        ]

    sweep_dir = bench.directory / "results" / f"sweep-{timestamp()}"
    sweep_dir.mkdir(parents=True)
    print(f"Resultados locales: {sweep_dir}", flush=True)

    results = []
    for config in configs:
        result = run_sweep_config(bench, args, config, sweep_dir)
        results.append(result)
        print(
            f"{config['label']}: {'PASS' if result['passed'] else 'FAIL'}",
            flush=True,
        )

    base_checksum = results[0]["checksum"]
    sweep = {
        "build": build_values(bench, args),
        "commit": git_commit(),
        "results": results,
        "passed": all(
            result["passed"] and result["checksum"] == base_checksum
            for result in results
        ),
    }
    (sweep_dir / "sweep.json").write_text(
        json.dumps(sweep, indent=2, ensure_ascii=False) + "\n"
    )
    write_sweep_summary(bench, sweep, sweep_dir)
    print(f"Resumen: {sweep_dir / 'summary.md'}")
    return 0 if sweep["passed"] else 1
