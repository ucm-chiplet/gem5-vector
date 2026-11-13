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

"""Contrasta Pathfinder en la VPU de gem5 con Ara RTL (Verilator).

Ejecuta el mismo kernel (un único ``kernel.o``) en Ara y en varios
escenarios de gem5, comprueba que ambos hacen el mismo trabajo y dan el
mismo resultado, y genera un informe con la diferencia de ciclos y su
descomposición. Ver ``documentacion/hito2/Pathfinder - comparación con
Ara RTL.md`` y ``ara/README.md``.

Pasos:

1. Compila los objetos comunes, el programa de gem5 y los de Ara
   (``ara/Makefile``) y comprueba con ``tools/compare_kernel.py`` que
   ``runVector`` es idéntico salvo direcciones y que los datos tienen el
   mismo desplazamiento en la línea de 64 bytes.
2. Simula Ara (``ara/run_ara.py``) y los escenarios de gem5 en paralelo.
   gem5 se ejecuta con ``--debug-flags=Exec,VectorTimeline``.
3. Comprueba resultado y trabajo, compara métricas y escribe
   ``results/ara-compare-<fecha>/`` (``summary.md``, ``results.json``,
   ``figures/``), ignorado por Git.

Uso:
  python3 benchmarks/pathfinder/compare_ara.py --input tiny \\
      --trace-python <python con pywellen y numpy>
  python3 benchmarks/pathfinder/compare_ara.py --input small ...
"""

import argparse
import collections
import concurrent.futures
import gzip
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

PATHFINDER = Path(__file__).resolve().parent
REPOSITORY = PATHFINDER.parents[1]
sys.path.insert(0, str(PATHFINDER.parent / "common"))
sys.path.insert(0, str(PATHFINDER / "ara"))

import run_ara  # noqa: E402
import vpu_bench  # noqa: E402

GEM5 = REPOSITORY / "build" / "RISCV" / "gem5.opt"
CONFIG = REPOSITORY / "configs" / "example" / "riscv_vpu.py"
ENGINE = "system.cpu.vectorEngine."
TICKS_PER_CYCLE = 1000  # riscv_vpu.py: CPU a 1 GHz, 1 tick = 1 ps

# Escenarios de gem5. memAra iguala la latencia por bloque de la memoria a
# la de Ara (calibrada en el documento: 14,7 ciclos frente a 14,0); banks8
# da a cada lane los 8 bancos de VRF de Ara; win8, sus 8 instrucciones en
# vuelo.
SCENARIOS = {
    "base": [],
    "memAra": ["--mem-latency=0ns"],
    "memAra+banks8": ["--mem-latency=0ns", "--vpu-set=vrf.banks_per_lane=8"],
    # Ventanas como Ara (NrVInsn = 8): cola, comandos activos, contextos
    # por lane y ventana vectorial de Minor.
    "memAra+banks8+win8": [
        "--mem-latency=0ns",
        "--vpu-set=vrf.banks_per_lane=8",
        "--vpu-command-queue-entries=8",
        "--vpu-max-active-commands=8",
        "--vpu-lane-task-entries=8",
        "--minor-vector-window=8",
    ],
}
NATIVE = "native"

EXEC_LINE = re.compile(
    r"^\s*(\d+): system\.cpu: T0 : 0x([0-9a-f]+) @runVector(?:\+\d+)?\s+:"
)
DISPATCH_LINE = re.compile(
    r"^\s*(\d+): system\.cpu\.vectorEngine: dispatch cmd=(\d+) "
    r"pc=0x([0-9a-f]+) vl=(\d+)"
)
START_LINE = re.compile(r"^\s*(\d+): vectorEngine\.sequencer: start cmd=(\d+)")
COMPLETE_LINE = re.compile(
    r"^\s*(\d+): system\.cpu\.vectorEngine: complete cmd=(\d+)"
)
CHECKSUM_LINE = re.compile(r"\[pathfinder\] checksum: (sum=\d+ xor=\d+)")
OBJDUMP_LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{4,8}\s+)+(\S.*)$")


# ---------------------------------------------------------------- utilidades


def kernel_listing(kernel_object):
    """{desplazamiento: texto} de runVector a partir de kernel.o."""
    text = subprocess.run(
        [
            "llvm-objdump",
            "-d",
            "--mattr=+v",
            "--no-show-raw-insn",
            str(kernel_object),
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    listing = {}
    for line in text.splitlines():
        match = re.match(r"^\s*([0-9a-f]+):\s+(\S.*)$", line)
        if match:
            listing[int(match.group(1), 16)] = " ".join(match.group(2).split())
    return listing


def mnemonic(text):
    return text.split()[0] if text else "?"


def is_vector_command(text):
    """Instrucción que llega a la VPU como comando (no vsetvli)."""
    name = mnemonic(text)
    return name.startswith("v") and not name.startswith("vset")


def retire_profile(commits):
    """{offset: [n, suma de ciclos desde la retirada anterior]}."""
    profile = collections.defaultdict(lambda: [0, 0.0])
    previous = None
    for cycle, offset in commits:
        if previous is not None:
            profile[offset][0] += 1
            profile[offset][1] += cycle - previous
        previous = cycle
    return profile


def mean(values):
    return statistics.fmean(values) if values else None


# ---------------------------------------------------------------- gem5


def run_gem5(name, extra, binary, out_dir, native, timeout):
    """Ejecuta gem5 con trazas Exec y VectorTimeline comprimidas."""
    out_dir.mkdir(parents=True, exist_ok=True)
    flags = "Exec" if native else "Exec,VectorTimeline"
    command = [
        str(GEM5),
        f"--outdir={out_dir}",
        f"--debug-flags={flags}",
        "--debug-file=trace.out.gz",
        str(CONFIG),
    ]
    if native:
        command.append("--no-offload")
    command += extra + [str(binary)]
    status, timed_out = vpu_bench.run_logged(
        command, out_dir / "simulation.log", timeout=timeout
    )
    log = (out_dir / "simulation.log").read_text(errors="replace")
    checksum = CHECKSUM_LINE.search(log)
    return {
        "name": name,
        "command": command,
        "returncode": status,
        "timed_out": timed_out,
        "passed": status == 0 and "[pathfinder] PASS" in log,
        "checksum": checksum.group(1) if checksum else None,
    }


def parse_gem5_trace(path, kernel_address):
    """Retiradas del kernel y línea temporal de comandos de la VPU."""
    commits = []
    dispatch, start, complete = {}, {}, {}
    with gzip.open(path, "rt", errors="replace") as stream:
        for line in stream:
            match = EXEC_LINE.match(line)
            if match:
                commits.append(
                    (
                        int(match.group(1)) / TICKS_PER_CYCLE,
                        int(match.group(2), 16) - kernel_address,
                    )
                )
                continue
            match = DISPATCH_LINE.match(line)
            if match:
                dispatch[int(match.group(2))] = (
                    int(match.group(1)) / TICKS_PER_CYCLE,
                    int(match.group(3), 16) - kernel_address,
                    int(match.group(4)),
                )
                continue
            match = START_LINE.match(line)
            if match:
                start[int(match.group(2))] = (
                    int(match.group(1)) / TICKS_PER_CYCLE
                )
                continue
            match = COMPLETE_LINE.match(line)
            if match:
                complete[int(match.group(2))] = (
                    int(match.group(1)) / TICKS_PER_CYCLE
                )
    timeline = []
    for command in sorted(dispatch):
        time, offset, vl = dispatch[command]
        timeline.append(
            {
                "offset": offset,
                "vl": vl,
                "dispatch": time,
                "start": start.get(command),
                "end": complete.get(command),
            }
        )
    return commits, timeline


def gem5_metrics(stats, nr_lanes):
    """Métricas de la región del kernel (primer volcado de stats.txt)."""

    def get(name):
        return vpu_bench.metric(stats, name)

    def cycles(name):
        value = get(name)
        return None if value is None else value / TICKS_PER_CYCLE

    m = {
        "roi_cycles": cycles("simTicks"),
        "instructions": get("system.cpu.commitStats0.numInsts"),
        "commands_accepted": get(ENGINE + "commands.accepted"),
        "commands_busy_cycles": cycles(ENGINE + "commands.busyTicks"),
        "sequencer_busy_cycles": cycles(ENGINE + "sequencer.busyTicks"),
        "sequencer_peak_outstanding": get(
            ENGINE + "sequencer.peakOutstanding"
        ),
        "sequencer_overlapped_starts": get(
            ENGINE + "sequencer.overlappedStarts"
        ),
        "sequencer_multi_active_cycles": cycles(
            ENGINE + "sequencer.multiActiveTicks"
        ),
        "hazard_wait_cycles": cycles(ENGINE + "sequencer.hazardWaitTicks"),
        "vlsu_busy_cycles": cycles(ENGINE + "vlsu.busyTicks"),
        "vlsu_memory_cycles": cycles(ENGINE + "vlsu.memoryTicks"),
        "vlsu_blocks": get(ENGINE + "vlsu.blocks"),
        "load_bytes": get(ENGINE + "vlsu.loadBytes"),
        "store_bytes": get(ENGINE + "vlsu.storeBytes"),
        "slide_busy_cycles": cycles(ENGINE + "slide.busyTicks"),
        "lanes": [],
    }
    for lane in range(nr_lanes):
        base = f"{ENGINE}lane{lane}."
        lrf = f"{ENGINE}lrf{lane}."
        banks = []
        bank = 0
        while get(f"{lrf}bank{bank}.accepted") is not None:
            banks.append(get(f"{lrf}bank{bank}.accepted"))
            bank += 1
        m["lanes"].append(
            {
                "alu_elements": get(base + "alu*.elements"),
                "alu_busy_cycles": cycles(base + "alu0.busyTicks"),
                "vrf_reads": get(lrf + "reads"),
                "vrf_writes": get(lrf + "writes"),
                "vrf_bank_accesses": banks,
            }
        )
    return m


# ---------------------------------------------------------------- análisis


def vector_profile_ara(commits, listing, timeline):
    """Por desplazamiento: espera hasta asignación y residencia en Ara."""
    vector = [o for _, o in commits if is_vector_command(listing.get(o, ""))]
    if len(vector) != len(timeline):
        return None
    profile = collections.defaultdict(lambda: {"wait": [], "total": []})
    for offset, event in zip(vector, timeline):
        profile[offset]["wait"].append(event["start"] - event["request"])
        profile[offset]["total"].append(event["end"] - event["request"])
    return {o: {k: mean(v) for k, v in p.items()} for o, p in profile.items()}


def vector_profile_gem5(timeline):
    profile = collections.defaultdict(lambda: {"wait": [], "total": []})
    for event in timeline:
        if event["start"] is None or event["end"] is None:
            continue
        profile[event["offset"]]["wait"].append(
            event["start"] - event["dispatch"]
        )
        profile[event["offset"]]["total"].append(
            event["end"] - event["dispatch"]
        )
    return {o: {k: mean(v) for k, v in p.items()} for o, p in profile.items()}


def mix(commits, listing):
    counter = collections.Counter(
        mnemonic(listing.get(offset, "?")) for _, offset in commits
    )
    return dict(sorted(counter.items()))


def vector_mix(commits, listing):
    return {
        k: v for k, v in mix(commits, listing).items() if k.startswith("v")
    }


# ---------------------------------------------------------------- informe


def fmt(value, digits=1):
    """Número con separador de miles espacio y coma decimal."""
    if value is None:
        return "—"
    if isinstance(value, float):
        if digits == 0:
            value = round(value)
        else:
            text = f"{value:,.{digits}f}"
            return text.replace(",", " ").replace(".", ",")
    if isinstance(value, int):
        return f"{value:,}".replace(",", " ")
    return str(value)


def percent(part, whole):
    if part is None or not whole:
        return "—"
    return fmt(100 * part / whole) + " %"


def figures(report, out_dir):
    """Figuras SVG; se omiten si no hay matplotlib."""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return []
    out_dir.mkdir(exist_ok=True)
    created = []
    names = list(SCENARIOS)
    strips = report["strips"]

    # 1. Ciclos por tira: escenarios de gem5 y Ara.
    labels = [f"gem5 {n}" for n in names] + ["Ara RTL"]
    values = [
        report["gem5"][n]["metrics"]["roi_cycles"] / strips for n in names
    ]
    values.append(report["ara"]["kernel_cycles"] / strips)
    fig, ax = plt.subplots(figsize=(7, 3.6))
    colors = ["#4C72B0"] * len(names) + ["#DD8452"]
    bars = ax.bar(labels, values, color=colors)
    for bar, value in zip(bars, values):
        ax.annotate(
            fmt(value),
            (bar.get_x() + bar.get_width() / 2, value),
            ha="center",
            va="bottom",
            fontsize=9,
        )
    for previous, current in zip(
        range(len(values) - 1), range(1, len(values))
    ):
        delta = values[current] - values[previous]
        ax.annotate(
            "Δ " + ("+" if delta >= 0 else "−") + fmt(abs(delta)),
            (
                (previous + current) / 2,
                max(values[previous], values[current]) * 1.05,
            ),
            ha="center",
            fontsize=8,
            color="#555555",
        )
    ax.set_ylabel("Ciclos por tira")
    ax.set_title(f"Pathfinder {report['input']}: ciclos por tira del kernel")
    ax.set_ylim(0, max(values) * 1.18)
    ax.spines[["top", "right"]].set_visible(False)
    fig.tight_layout()
    path = out_dir / "cycles-per-strip.svg"
    fig.savefig(path)
    plt.close(fig)
    created.append(path.name)

    # 2. Retirada por instrucción del bucle de tiras.
    rows = report["strip_profile"]
    if rows:
        fig, ax = plt.subplots(figsize=(8, 0.28 * len(rows) + 1.2))
        y = range(len(rows))
        series = [("Ara RTL", "ara", "#DD8452")] + [
            (f"gem5 {n}", n, c)
            for n, c in zip(
                names, ["#4C72B0", "#55A868", "#8172B3", "#937860"]
            )
        ]
        height = 0.8 / len(series)
        for index, (label, key, color) in enumerate(series):
            ax.barh(
                [i + index * height for i in y],
                [row[key] or 0 for row in rows],
                height=height,
                label=label,
                color=color,
            )
        ax.set_yticks([i + 0.4 - height / 2 for i in y])
        ax.set_yticklabels(
            [f"+{row['offset']:#x} {row['instruction']}" for row in rows],
            fontsize=7,
        )
        ax.invert_yaxis()
        ax.set_xlabel("Ciclos medios desde la retirada anterior")
        ax.set_title("Dónde se espera dentro de una tira")
        ax.legend(fontsize=8)
        ax.spines[["top", "right"]].set_visible(False)
        fig.tight_layout()
        path = out_dir / "strip-profile.svg"
        fig.savefig(path)
        plt.close(fig)
        created.append(path.name)
    return created


def write_summary(report, out_dir):
    names = list(SCENARIOS)
    ara = report["ara"]
    am = ara["metrics"]
    strips = report["strips"]
    g = {n: report["gem5"][n]["metrics"] for n in names}
    lines = [
        f"# Pathfinder: VPU de gem5 frente a Ara RTL ({report['input']})",
        "",
        "Resultado de las comprobaciones: "
        + (
            "**todas correctas**."
            if report["checks_passed"]
            else "**hay comprobaciones fallidas; no interpretar los tiempos**."
        ),
        "",
        "## Condiciones",
        "",
        "| Dato | Valor |",
        "| --- | --- |",
        f"| Entrada | `{report['input']}` ({report['rows']}×{report['cols']}), "
        f"`NUM_RUNS={report['num_runs']}`, datos alineados a "
        f"{report['align']} bytes |",
        f"| Commit gem5 | `{report['gem5_commit']}` |",
        f"| Ara | `{report['ara_commit']}`, {ara['nr_lanes']} lanes, VLEN "
        f"{ara['vlen']}, modelo `{ara['simulator']}` |",
        f"| Kernel | `kernel.o` común: {report['kernel_check']['instructions']} "
        f"instrucciones, {report['kernel_check']['relocated_instructions']} "
        "con relocación |",
        f"| Tiras | {strips} (una por `vslide1up.vx`) |",
        "",
        "## Comprobaciones",
        "",
        "| Comprobación | Estado | Detalle |",
        "| --- | --- | --- |",
    ]
    for check in report["checks"]:
        state = "correcta" if check["passed"] else "**FALLA**"
        lines.append(f"| {check['name']} | {state} | {check['detail']} |")

    ara_cycles = ara["kernel_cycles"]
    lines += [
        "",
        "## Ciclos del kernel",
        "",
        "Ciclos de `runVector()`: en Ara, CSR `cycle` alrededor de la llamada "
        "(tras un `fence`); en gem5, ticks entre `m5_reset_stats` y "
        "`m5_dump_stats` a 1 GHz.",
        "",
        "| Plataforma | Ciclos | Ciclos por tira | Respecto a Ara |",
        "| --- | --- | --- | --- |",
        f"| Ara RTL | {fmt(ara_cycles)} | {fmt(ara_cycles / strips)} | 1,00 |",
    ]
    for name in names:
        cycles = g[name]["roi_cycles"]
        lines.append(
            f"| gem5 `{name}` (offload) | {fmt(cycles, 0)} | "
            f"{fmt(cycles / strips)} | {fmt(cycles / ara_cycles, 2)} |"
        )
    native = report["gem5"].get(NATIVE)
    if native:
        cycles = native["metrics"]["roi_cycles"]
        lines.append(
            f"| gem5 nativo (RVV en Minor, referencia) | {fmt(cycles, 0)} | "
            f"{fmt(cycles / strips)} | {fmt(cycles / ara_cycles, 2)} |"
        )
    lines += [
        "",
        "Pasos (ciclos por tira): "
        + " → ".join(
            f"`{n}` {fmt(g[n]['roi_cycles'] / strips)}" for n in names
        )
        + f" → Ara {fmt(ara_cycles / strips)}.",
    ]
    if report.get("figures"):
        lines += ["", "![Ciclos por tira](figures/cycles-per-strip.svg)"]

    # Retirada por instrucción.
    lines += [
        "",
        "## Dónde se espera dentro de una tira",
        "",
        "Ciclos medios entre la retirada de la instrucción anterior y la de "
        "cada instrucción del bucle de tiras (las que se ejecutan una vez "
        "por tira). El código es el mismo en ambas plataformas. En Minor "
        "una instrucción vectorial se retira después de que la VPU la "
        "complete, así que su fila incluye su ejecución; en CVA6+Ara se "
        "retira al aceptarla Ara y la espera aparece en la instrucción que "
        "la necesita (por ejemplo, el `lw` tras el `vse32.v`). Las filas "
        "explican dónde se espera en cada plataforma; la comparación "
        "directa es la de las sumas. La última fila reparte por tira el "
        "código que no es del bucle de tiras (cambio de fila y copia "
        "inicial).",
        "",
        "| Desp. | Instrucción | Ara | "
        + " | ".join(f"`{n}`" for n in names)
        + " |",
        "| --- | --- | --- | " + " | ".join("---" for _ in names) + " |",
    ]
    totals = collections.Counter()
    for row in report["strip_profile"]:
        lines.append(
            f"| +{row['offset']:#x} | `{row['instruction']}` | "
            f"{fmt(row['ara'])} | "
            + " | ".join(fmt(row[n]) for n in names)
            + " |"
        )
        totals["ara"] += row["ara"] or 0
        for n in names:
            totals[n] += row[n] or 0
    lines.append(
        "| | **Suma del bucle de tiras** | **"
        + fmt(totals["ara"])
        + "** | "
        + " | ".join(f"**{fmt(totals[n])}**" for n in names)
        + " |"
    )
    lines.append(
        "| | Resto por tira (fila y copia inicial) | "
        + fmt(ara_cycles / strips - totals["ara"])
        + " | "
        + " | ".join(
            fmt(g[n]["roi_cycles"] / strips - totals[n]) for n in names
        )
        + " |"
    )
    if report.get("figures") and "strip-profile.svg" in report["figures"]:
        lines += ["", "![Retirada por instrucción](figures/strip-profile.svg)"]

    # Residencia en la VPU.
    lines += [
        "",
        "## Instrucciones vectoriales en la VPU",
        "",
        "Medias por instrucción del kernel. **Entrada**: en Ara, de la "
        "petición de CVA6 a la asignación en el secuenciador; en gem5, de "
        "la entrega (`dispatch`) al inicio en el secuenciador. **Total**: "
        "de la petición o entrega al fin de la instrucción. Ara asigna sin "
        "esperar dependencias (encadena); gem5 inicia cuando no hay "
        "dependencias pendientes (sin chaining).",
        "",
        "| Desp. | Instrucción | Ara entrada | Ara total | "
        + " | ".join(f"`{n}` entrada | `{n}` total" for n in names)
        + " |",
        "| --- | --- | --- | --- | "
        + " | ".join("--- | ---" for _ in names)
        + " |",
    ]
    for row in report["vector_profile"]:
        cells = [
            f"+{row['offset']:#x}",
            f"`{row['instruction']}`",
            fmt(row["ara"]["wait"]) if row["ara"] else "—",
            fmt(row["ara"]["total"]) if row["ara"] else "—",
        ]
        for n in names:
            entry = row[n]
            cells += [
                fmt(entry["wait"]) if entry else "—",
                fmt(entry["total"]) if entry else "—",
            ]
        lines.append("| " + " | ".join(cells) + " |")

    # Recursos emparejados.
    lane_a = am["lanes"][0]
    lane_g = {n: g[n]["lanes"][0] for n in names}
    window = am["window"]["cycles"]

    def row(label, ara_value, gem5_values, definition, same):
        return (
            f"| {label} | {ara_value} | "
            + " | ".join(gem5_values)
            + f" | {'igual' if same else 'distinta'} | {definition} |"
        )

    axi = am["axi"]
    read_last = (axi.get("read_latency_ar_to_last_r") or {}).get("median")
    write = (axi.get("write_latency_aw_to_b") or {}).get("median")
    loads = axi["ar_bursts"]
    stores = axi["aw_bursts"]
    ara_block = None
    if read_last is not None and write is not None and loads + stores:
        ara_block = (loads * read_last + stores * write) / (loads + stores)
    lines += [
        "",
        "## Recursos emparejados",
        "",
        f"Ara: ventana vectorial de {fmt(window)} ciclos (de la primera "
        "instrucción aceptada a Ara inactiva). gem5: región del kernel. "
        "La columna *Definición* indica si ambos lados miden lo mismo; "
        "las filas *distinta* sólo se comparan en tendencia.",
        "",
        "| Métrica | Ara | "
        + " | ".join(f"`{n}`" for n in names)
        + " | Definición | Qué mide en cada lado |",
        "| --- | --- | " + " | ".join("---" for _ in names) + " | --- | --- |",
        row(
            "Instrucciones vectoriales en vuelo, máximo",
            fmt(am["sequencer"]["outstanding_peak"]),
            [fmt(g[n]["sequencer_peak_outstanding"]) for n in names],
            "Ara: identificadores ocupados en el secuenciador; gem5: "
            "comandos ejecutándose (`sequencer.peakOutstanding`)",
            False,
        ),
        row(
            "Instrucciones vectoriales en vuelo, media",
            fmt(am["sequencer"]["outstanding_avg"], 2),
            [
                fmt(
                    (g[n]["sequencer_busy_cycles"] or 0) / g[n]["roi_cycles"],
                    2,
                )
                for n in names
            ],
            "gem5: como máximo 1 en ejecución, así que la media es la "
            "fracción de ciclos con alguno",
            False,
        ),
        row(
            "Ciclos con alguna instrucción vectorial en la VPU",
            percent(am["sequencer"]["busy_cycles_any_insn"], window),
            [
                percent(g[n]["commands_busy_cycles"], g[n]["roi_cycles"])
                for n in names
            ],
            "Ara: `vinsn_running_q`≠0; gem5: `commands.busyTicks`",
            True,
        ),
        row(
            "Unidad de memoria ocupada",
            percent(
                max(
                    am["units"]["load"]["busy_cycles"],
                    am["units"]["store"]["busy_cycles"],
                ),
                window,
            )
            + " (máx. load/store)",
            [
                percent(g[n]["vlsu_busy_cycles"], g[n]["roi_cycles"])
                for n in names
            ],
            "Ara: load y store son unidades separadas; gem5: una VLSU",
            False,
        ),
        row(
            "Slide ocupada",
            percent(am["units"]["slide"]["busy_cycles"], window),
            [
                percent(g[n]["slide_busy_cycles"], g[n]["roi_cycles"])
                for n in names
            ],
            "Ara: instrucción de slide asignada, incluida la espera por "
            "operandos encadenados; gem5: `slide.busyTicks`, tareas "
            "iniciadas sin dependencias",
            False,
        ),
        row(
            "Latencia de memoria por bloque (ciclos)",
            fmt(ara_block) + f" (R {fmt(read_last, 0)}, W {fmt(write, 0)})",
            [
                fmt(g[n]["vlsu_memory_cycles"] / g[n]["vlsu_blocks"])
                for n in names
            ],
            "Ara: AR→último R y AW→B, ponderado; gem5: "
            "`vlsu.memoryTicks / blocks`",
            True,
        ),
        row(
            "Bytes leídos / escritos por la VPU",
            f"{fmt(axi['r_beats'] * axi['beat_bytes'])} / "
            f"{fmt(axi['w_beats'] * axi['beat_bytes'])}",
            [
                f"{fmt(g[n]['load_bytes'])} / {fmt(g[n]['store_bytes'])}"
                for n in names
            ],
            "Ara: beats AXI × 8 B; gem5: `vlsu.load/storeBytes`",
            True,
        ),
        row(
            "Lane 0: lecturas / escrituras del VRF",
            f"{fmt(lane_a['vrf_reads'])} / {fmt(lane_a['vrf_writes'])}",
            [
                f"{fmt(lane_g[n]['vrf_reads'])} / {fmt(lane_g[n]['vrf_writes'])}"
                for n in names
            ],
            "Palabras de 64 bits concedidas por el VRF",
            True,
        ),
        row(
            "Lane 0: accesos por banco del VRF",
            " ".join(str(v) for v in lane_a["vrf_bank_accesses"]),
            [
                " ".join(str(v) for v in lane_g[n]["vrf_bank_accesses"])
                for n in names
            ],
            "Accesos concedidos a cada banco",
            True,
        ),
        row(
            "Lane 0: elementos de ALU",
            fmt(2 * lane_a["alu_result_words"]),
            [fmt(lane_g[n]["alu_elements"]) for n in names],
            "Ara: palabras de resultado × 2 (SEW 32); gem5: `alu*.elements`",
            True,
        ),
        row(
            "Lane 0: ALU ocupada",
            percent(lane_a["alu_result_words"], window)
            + " (ciclos con resultado)",
            [
                percent(lane_g[n]["alu_busy_cycles"], g[n]["roi_cycles"])
                for n in names
            ],
            "Ara: ciclos con palabra de resultado; gem5: `alu0.busyTicks`",
            False,
        ),
        row(
            "CPU esperando a la VPU para entregar",
            percent(am["sequencer"]["req_stall_cycles"], window),
            ["—" for _ in names],
            "Ara: petición de CVA6 sin aceptar; en gem5 la espera está "
            "en la retirada (ver tabla por instrucción)",
            False,
        ),
    ]

    lines += [
        "",
        "## Archivos",
        "",
        "- `results.json`: todo lo anterior y las comprobaciones.",
        "- `kernel-check.json`: comparación del kernel y de los datos.",
        "- `ara/`: `ara.json`, `build.log` y, por simulación (`full`, "
        "`traced`), `sim.log`; en `traced`, `metrics.json` y "
        "`kernel-commits.json`.",
        "- `gem5/<escenario>/`: `simulation.log`, `stats.txt`, `config.ini`.",
        "- `figures/`: figuras SVG.",
        "",
    ]
    (out_dir / "summary.md").write_text("\n".join(lines))


# ---------------------------------------------------------------- principal


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    run_ara.add_arguments(parser)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--gem5-timeout", type=int, default=7200)
    parser.add_argument("--no-native", action="store_true")
    parser.add_argument("--out", type=Path)
    args = run_ara.resolve(parser.parse_args())

    out_dir = args.out or (
        PATHFINDER / "results" / f"ara-compare-{vpu_bench.timestamp()}"
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Resultados: {out_dir}")

    # 1. Compilación y kernel común.
    log = out_dir / "build.log"
    if not run_ara.build(args, log):
        print("Fallo de compilación; ver build.log")
        return 1
    files = run_ara.paths(args)
    kernel_json = out_dir / "kernel-check.json"
    kernel_result = subprocess.run(
        [
            sys.executable,
            str(PATHFINDER / "tools" / "compare_kernel.py"),
            "--json",
            str(kernel_json),
            str(files["kernel_object"]),
            str(files["gem5_binary"]),
            str(files["ara_full"]),
            str(files["ara_traced"]),
        ],
        capture_output=True,
        text=True,
    )
    with log.open("a") as stream:
        stream.write(kernel_result.stdout + kernel_result.stderr)
    kernel_check = json.loads(kernel_json.read_text())
    if not kernel_check["passed"]:
        print("El kernel difiere entre binarios; no se simula.")
        print(kernel_result.stderr)
        return 1
    listing = kernel_listing(files["kernel_object"])
    gem5_kernel, _ = run_ara.symbol(files["gem5_binary"])

    # 2. Simulaciones en paralelo.
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs["ara"] = pool.submit(run_ara.run, args, out_dir / "ara")
        runs = dict(SCENARIOS)
        if not args.no_native:
            runs[NATIVE] = []
        for name, extra in runs.items():
            jobs[name] = pool.submit(
                run_gem5,
                name,
                extra,
                files["gem5_binary"],
                out_dir / "gem5" / name,
                name == NATIVE,
                args.gem5_timeout,
            )
        results = {name: job.result() for name, job in jobs.items()}

    ara_report = results.pop("ara")
    if "error" in ara_report:
        print("Ara: " + ara_report["error"])
        return 1
    ara_dir = out_dir / "ara" / "traced"
    ara_metrics = json.loads((ara_dir / "metrics.json").read_text())
    ara_commits = json.loads((ara_dir / "kernel-commits.json").read_text())

    gem5 = {}
    for name, result in results.items():
        run_dir = out_dir / "gem5" / name
        dumps = vpu_bench.read_dumps(run_dir / "stats.txt")
        commits, timeline = parse_gem5_trace(
            run_dir / "trace.out.gz", gem5_kernel
        )
        if not args.keep_trace:
            (run_dir / "trace.out.gz").unlink(missing_ok=True)
        gem5[name] = dict(result)
        gem5[name]["metrics"] = gem5_metrics(
            dumps[0] if dumps else {}, args.nr_lanes
        )
        gem5[name]["commits"] = commits
        gem5[name]["timeline"] = timeline

    # 3. Comprobaciones.
    checks = []

    def check(name, passed, detail):
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    check(
        "Kernel idéntico salvo direcciones",
        kernel_check["passed"],
        f"{kernel_check['instructions']} instrucciones en kernel.o, gem5 y "
        "los dos binarios de Ara",
    )
    offsets = {
        name: entry["data_line_offsets"]
        for name, entry in kernel_check["binaries"].items()
    }
    check(
        "Mismo desplazamiento de los datos en la línea de 64 B",
        True,
        ", ".join(f"{k}={v}" for k, v in next(iter(offsets.values())).items()),
    )
    full, traced = ara_report["full"], ara_report["traced"]
    check(
        "Ara: PASS (con referencia escalar) y fin correcto",
        full["passed"] and full["success"],
        full["checksum"] or "sin huella",
    )
    check(
        "Ara trazado: PASS y mismos ciclos de kernel que sin traza",
        traced["passed"] and traced["kernel_cycles"] == full["kernel_cycles"],
        f"{traced['kernel_cycles']} frente a {full['kernel_cycles']} ciclos",
    )
    for name in gem5:
        check(
            f"gem5 `{name}`: PASS",
            gem5[name]["passed"],
            gem5[name]["checksum"] or "sin huella",
        )
    checksums = {full["checksum"], traced["checksum"]} | {
        gem5[n]["checksum"] for n in gem5
    }
    check(
        "Misma huella en todas las ejecuciones",
        len(checksums) == 1,
        ", ".join(sorted(str(c) for c in checksums)),
    )
    ara_mix = mix(ara_commits, listing)
    for name in gem5:
        gem5_mix = mix(gem5[name]["commits"], listing)
        check(
            f"Mismas instrucciones retiradas en `runVector` (gem5 `{name}`)",
            gem5_mix == ara_mix,
            (
                f"Ara {len(ara_commits)}, gem5 {len(gem5[name]['commits'])}; "
                "mismo recuento por mnemónico"
                if gem5_mix == ara_mix
                else f"Ara {ara_mix} / gem5 {gem5_mix}"
            ),
        )
    vector_commands = sum(
        1 for _, o in ara_commits if is_vector_command(listing.get(o, ""))
    )
    check(
        "Ara: instrucciones aceptadas = vectoriales retiradas (sin vsetvli)",
        ara_metrics["sequencer"]["insns_accepted"] == vector_commands,
        f"{ara_metrics['sequencer']['insns_accepted']} / {vector_commands}",
    )
    for name in SCENARIOS:
        accepted = gem5[name]["metrics"]["commands_accepted"]
        check(
            f"gem5 `{name}`: comandos = vectoriales retiradas (sin vsetvli)",
            accepted == vector_commands,
            f"{accepted} / {vector_commands}",
        )
        check(
            f"gem5 `{name}`: línea temporal completa",
            len(gem5[name]["timeline"]) == vector_commands
            and all(
                e["start"] is not None and e["end"] is not None
                for e in gem5[name]["timeline"]
            ),
            f"{len(gem5[name]['timeline'])} comandos",
        )
    axi = ara_metrics["axi"]
    for name in SCENARIOS:
        m = gem5[name]["metrics"]
        same = (
            axi["r_beats"] * axi["beat_bytes"] == m["load_bytes"]
            and axi["w_beats"] * axi["beat_bytes"] == m["store_bytes"]
        )
        check(
            f"Mismos bytes de memoria vectorial (gem5 `{name}`)",
            same,
            f"Ara {axi['r_beats'] * axi['beat_bytes']}/"
            f"{axi['w_beats'] * axi['beat_bytes']}, gem5 "
            f"{m['load_bytes']}/{m['store_bytes']}",
        )
        lanes_same = all(
            a["vrf_reads"] == b["vrf_reads"]
            and a["vrf_writes"] == b["vrf_writes"]
            and 2 * a["alu_result_words"] == b["alu_elements"]
            for a, b in zip(ara_metrics["lanes"], m["lanes"])
        )
        check(
            f"Mismos accesos al VRF y elementos de ALU por lane (gem5 "
            f"`{name}`)",
            lanes_same,
            "lecturas, escrituras y elementos por lane",
        )

    # 4. Análisis.
    slide_offsets = [
        o for o, t in listing.items() if mnemonic(t) == "vslide1up.vx"
    ]
    strips = sum(1 for _, o in ara_commits if o in slide_offsets)
    ara_profile = retire_profile(ara_commits)
    gem5_profiles = {n: retire_profile(gem5[n]["commits"]) for n in SCENARIOS}
    strip_rows = []
    for offset in sorted(listing):
        count = ara_profile.get(offset, [0, 0])[0]
        if count < strips or count > strips + strips // 8:
            continue  # sólo el bucle de tiras
        row_data = {
            "offset": offset,
            "instruction": listing[offset],
            "count": count,
            "ara": ara_profile[offset][1] / count,
        }
        for name, profile in gem5_profiles.items():
            n, total = profile.get(offset, [0, 0])
            row_data[name] = total / n if n else None
        strip_rows.append(row_data)

    ara_vector = vector_profile_ara(
        ara_commits, listing, ara_metrics["timeline"]
    )
    gem5_vector = {
        n: vector_profile_gem5(gem5[n]["timeline"]) for n in SCENARIOS
    }
    vector_rows = []
    for offset in sorted(listing):
        if not is_vector_command(listing[offset]):
            continue
        entry = {
            "offset": offset,
            "instruction": listing[offset],
            "ara": (ara_vector or {}).get(offset),
        }
        for name in SCENARIOS:
            entry[name] = gem5_vector[name].get(offset)
        vector_rows.append(entry)

    header = (files["shared"] / "pathfinder_input.h").read_text()
    rows = int(re.search(r"PATHFINDER_ROWS (\d+)", header).group(1))
    cols = int(re.search(r"PATHFINDER_COLS (\d+)", header).group(1))
    ara_commit = (
        subprocess.run(
            [
                "git",
                "-C",
                str(args.ara_root),
                "describe",
                "--always",
                "--dirty",
                "--abbrev=10",
            ],
            capture_output=True,
            text=True,
        ).stdout.strip()
        or "unknown"
    )

    report = {
        "input": args.input,
        "rows": rows,
        "cols": cols,
        "num_runs": args.num_runs,
        "align": args.align,
        "gem5_commit": vpu_bench.git_commit(),
        "ara_commit": ara_commit,
        "kernel_check": kernel_check,
        "strips": strips,
        "checks": checks,
        "checks_passed": all(c["passed"] for c in checks),
        "ara": {
            "nr_lanes": args.nr_lanes,
            "vlen": args.vlen,
            "simulator": str(args.simulator),
            "kernel_cycles": full["kernel_cycles"],
            "full": full,
            "traced": traced,
            "metrics": {
                k: v for k, v in ara_metrics.items() if k != "timeline"
            },
            "vector_mix": vector_mix(ara_commits, listing),
        },
        "gem5": {
            name: {
                k: v
                for k, v in entry.items()
                if k not in ("commits", "timeline")
            }
            for name, entry in gem5.items()
        },
        "strip_profile": strip_rows,
        "vector_profile": vector_rows,
    }
    report["figures"] = figures(report, out_dir / "figures")
    (out_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    write_summary(report, out_dir)

    for item in checks:
        print(("OK    " if item["passed"] else "FALLA ") + item["name"])
    print(
        f"Ara {full['kernel_cycles']} ciclos; "
        + ", ".join(
            f"gem5 {n} {gem5[n]['metrics']['roi_cycles']:.0f}"
            for n in SCENARIOS
        )
    )
    print(f"Resumen: {out_dir / 'summary.md'}")
    return 0 if report["checks_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
