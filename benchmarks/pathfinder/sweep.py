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

"""Barre configuraciones de la VPU con Pathfinder en modo offload.

Cada configuración se ejecuta con ``run.py --mode offload`` en su propio
subdirectorio. La primera es la referencia: las demás deben dar la misma
huella del resultado, y su tiempo y sus métricas se comparan con ella.
Sirve para comprobar que cambiar un recurso tiene efectos observables y
conserva el resultado funcional (criterio 5 del hito 2).

La lista por defecto varía un eje cada vez. ``--configs`` acepta un JSON
con una lista de objetos ``{"label": ..., "args": [...]}``, donde ``args``
son opciones de ``configs/example/riscv_vpu.py``.
"""

import argparse
import json
import subprocess
import sys
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path

import run as pathfinder_run

BENCHMARK = Path(__file__).resolve().parent
CPU = pathfinder_run.CPU
ENGINE = pathfinder_run.ENGINE

DEFAULT_CONFIGS = (
    {"label": "base", "args": []},
    {"label": "alus2", "args": ["--vpu-set=lane.alus_per_lane=2"]},
    {"label": "banks4", "args": ["--vpu-set=vrf.banks_per_lane=4"]},
    {"label": "rw-ports2", "args": ["--vpu-set=bank.read_write_ports=2"]},
    {"label": "lanes4", "args": ["--vpu-set=vrf.num_lanes=4"]},
    {
        "label": "transit4",
        "args": ["--vpu-set=interconnect.transit_latency=4"],
    },
    {"label": "block32", "args": ["--vpu-set=vlsu.max_block_bytes=32"]},
    {"label": "window4", "args": ["--minor-vector-window=4"]},
    {"label": "active1", "args": ["--vpu-set=max_active_commands=1"]},
)

# Métricas de la región del kernel que compara la tabla. Los patrones con
# comodines suman todas las instancias. Son tiempos de ocupación por
# recurso: pueden solaparse entre sí y no suman el tiempo total.
SWEEP_METRICS = (
    ("Ticks", "simTicks"),
    ("Máx. en VPU", ENGINE + "commands.peakOutstanding"),
    ("Máx. ejecutándose", ENGINE + "sequencer.peakOutstanding"),
    ("Inicios solapados", ENGINE + "sequencer.overlappedStarts"),
    ("VLSU ocupada", ENGINE + "vlsu.busyTicks"),
    ("ALUs ocupadas", ENGINE + "lane*.alu*.busyTicks"),
    ("Espera VRF", ENGINE + "lrf*.grantWaitTicks"),
    ("Slide ocupada", ENGINE + "slide.busyTicks"),
    ("Red: transporte", ENGINE + "interconnect.transportTicks"),
    ("Red: remotos", ENGINE + "interconnect.remotePackets"),
)


def run_config(args, config, sweep_dir):
    """Ejecuta una configuración y devuelve su resumen."""
    out_dir = sweep_dir / config["label"]
    command = [
        sys.executable,
        str(BENCHMARK / "run.py"),
        "--mode=offload",
        f"--input={args.input}",
        f"--runs={args.runs}",
        f"--timeout={args.timeout}",
        f"--gem5={args.gem5}",
        f"--results-dir={out_dir}",
    ]
    command.extend(f"--config-arg={item}" for item in config["args"])
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
    dumps = pathfinder_run.read_dumps(out_dir / "offload/stats.txt")
    roi = dumps[0] if len(dumps) >= 2 else {}
    result["metrics"] = {
        label: pathfinder_run.metric(roi, pattern, offload=True)
        for label, pattern in SWEEP_METRICS
    }
    return result


def relative(value, reference):
    """Variación porcentual respecto a la referencia, como texto."""
    if not isinstance(value, (int, float)) or not reference:
        return "-"
    change = 100.0 * (value - reference) / reference
    return f"{change:+.1f} %"


def write_summary(sweep, sweep_dir):
    """Escribe summary.md con la tabla comparativa del barrido."""
    results = sweep["results"]
    base = results[0]
    base_ticks = base["metrics"].get("Ticks")
    lines = [
        "# Barrido de configuraciones con Pathfinder",
        "",
        f"Resultado global: **{'PASS' if sweep['passed'] else 'FAIL'}**.",
        "",
        f"Entrada `{sweep['input']}`, `NUM_RUNS={sweep['runs']}`, commit "
        f"`{sweep['commit']}`, modo offload. Referencia: `{base['label']}`.",
        "",
        "Métricas de la región del kernel, en ticks salvo recuentos. Los",
        "tiempos de ocupación de cada recurso se solapan y no suman el total.",
        "",
        "| Configuración | Resultado | Huella igual | Ticks | Δ ticks | "
        + " | ".join(label for label, _ in SWEEP_METRICS[1:])
        + " |",
        "| --- | --- | --- | --- | --- |"
        + " --- |" * (len(SWEEP_METRICS) - 1),
    ]
    for result in results:
        metrics = result["metrics"]
        same = result["checksum"] == base["checksum"]
        values = []
        for label, _ in SWEEP_METRICS[1:]:
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", choices=("tiny", "small"), default="tiny")
    parser.add_argument("--runs", type=int, default=1, help="NUM_RUNS")
    parser.add_argument(
        "--gem5",
        type=Path,
        default=pathfinder_run.REPOSITORY / "build/RISCV/gem5.opt",
    )
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument(
        "--configs", type=Path, help="JSON list of {label, args}"
    )
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        help="Run only these labels; the first config is always kept",
    )
    args = parser.parse_args()
    args.gem5 = args.gem5.expanduser().resolve()

    configs = list(DEFAULT_CONFIGS)
    if args.configs:
        configs = json.loads(args.configs.read_text())
    labels = [config["label"] for config in configs]
    if not configs or len(set(labels)) != len(labels):
        parser.error("la lista necesita etiquetas únicas y no vacía")
    if args.only:
        configs = [configs[0]] + [
            config for config in configs[1:] if config["label"] in args.only
        ]

    run_name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    sweep_dir = BENCHMARK / "results" / f"sweep-{run_name}"
    sweep_dir.mkdir(parents=True)
    print(f"Resultados locales: {sweep_dir}", flush=True)

    results = []
    for config in configs:
        result = run_config(args, config, sweep_dir)
        results.append(result)
        print(
            f"{config['label']}: {'PASS' if result['passed'] else 'FAIL'}",
            flush=True,
        )

    base_checksum = results[0]["checksum"]
    sweep = {
        "input": args.input,
        "runs": args.runs,
        "commit": pathfinder_run.git_commit(),
        "results": results,
        "passed": all(
            result["passed"] and result["checksum"] == base_checksum
            for result in results
        ),
    }
    (sweep_dir / "sweep.json").write_text(
        json.dumps(sweep, indent=2, ensure_ascii=False) + "\n"
    )
    write_summary(sweep, sweep_dir)
    print(f"Resumen: {sweep_dir / 'summary.md'}")
    return 0 if sweep["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
