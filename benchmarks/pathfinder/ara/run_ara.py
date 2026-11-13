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

"""Compila y simula Pathfinder en Ara RTL (Verilator) y extrae métricas.

Hace dos simulaciones del mismo kernel:

- ``full``: programa completo con la referencia escalar; aporta el
  resultado (``PASS`` y huella) y los ciclos del kernel.
- ``traced``: sin la referencia escalar (``SKIP_SCALAR=1``) y con traza
  FST; aporta las métricas de ``ara_trace.py``. Debe dar la misma huella
  y los mismos ciclos de kernel que ``full``.

De la traza de retirada de CVA6 (``trace_hart_0.dasm``) obtiene la
secuencia de instrucciones de ``runVector``, su ciclo de retirada y su
desplazamiento dentro del símbolo, que es el mismo que en gem5 porque los
dos programas enlazan el mismo ``kernel.o``.

Uso independiente (``compare_ara.py`` lo invoca como módulo):

  python3 benchmarks/pathfinder/ara/run_ara.py --input tiny \\
      --trace-python <python con pywellen y numpy>
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ARA_DIR = Path(__file__).resolve().parent
PATHFINDER = ARA_DIR.parent
DEFAULT_ARA_ROOT = (PATHFINDER / "../../../ara").resolve()

KERNEL_LINE = re.compile(r"\[pathfinder\] kernel cycles: (\d+)")
CHECKSUM_LINE = re.compile(r"\[pathfinder\] checksum: (sum=\d+ xor=\d+)")
PASS_LINE = re.compile(r"\[pathfinder\] PASS")
SUCCESS_LINE = re.compile(r"\*\*\* SUCCESS \*\*\*")
COMMIT_LINE = re.compile(r"^\s*(\d+)\s+0x([0-9a-f]+)\s")


def make(ara_root, values, target, log):
    """Ejecuta make en ara/ con las variables indicadas."""
    command = ["make", "-C", str(ARA_DIR), f"ARA_ROOT={ara_root}", target]
    command += [f"{name}={value}" for name, value in values.items()]
    with log.open("a") as stream:
        stream.write("$ " + " ".join(command) + "\n")
        stream.flush()
        result = subprocess.run(
            command, stdout=stream, stderr=subprocess.STDOUT, text=True
        )
    return result.returncode


def build_values(args, skip_scalar):
    return {
        "INPUT": args.input,
        "NUM_RUNS": args.num_runs,
        "ALIGN": args.align,
        "NR_LANES": args.nr_lanes,
        "VLEN": args.vlen,
        "SKIP_SCALAR": int(skip_scalar),
    }


def paths(args):
    """Rutas de los objetos y binarios que generan los Makefiles."""
    shared = (
        PATHFINDER
        / "build"
        / (f"{args.input}-runs{args.num_runs}-align{args.align}")
    )
    ara = f"ara-{args.nr_lanes}l-v{args.vlen}"
    return {
        "shared": shared,
        "kernel_object": shared / "kernel.o",
        "gem5_binary": shared / "pathfinder",
        "ara_full": shared / ara / "pathfinder",
        "ara_traced": shared / f"{ara}-noscalar" / "pathfinder",
    }


def build(args, log):
    """Compila ambos binarios de Ara (y, de paso, el de gem5)."""
    for skip in (False, True):
        if make(args.ara_root, build_values(args, skip), "all", log) != 0:
            return False
    return True


def symbol(binary, name="runVector"):
    """(dirección, tamaño) de un símbolo, con llvm-readelf."""
    text = subprocess.run(
        ["llvm-readelf", "-s", "-W", str(binary)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[-1] == name:
            return int(fields[1], 16), int(fields[2])
    raise ValueError(f"{binary}: symbol {name} not found")


def simulate(simulator, binary, run_dir, trace, timeout):
    """Simula binary en Verilator dentro de run_dir y lee su salida."""
    run_dir.mkdir(parents=True, exist_ok=True)
    command = [str(simulator)]
    if trace:
        command.append("-t")
    command += ["-l", f"ram,{binary},elf"]
    log = run_dir / "sim.log"
    with log.open("w") as stream:
        try:
            result = subprocess.run(
                command,
                cwd=run_dir,
                stdout=stream,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = None
    text = log.read_text(errors="replace")
    kernel = KERNEL_LINE.search(text)
    checksum = CHECKSUM_LINE.search(text)
    return {
        "command": command,
        "returncode": status,
        "success": bool(SUCCESS_LINE.search(text)),
        "passed": bool(PASS_LINE.search(text)) and status == 0,
        "kernel_cycles": int(kernel.group(1)) if kernel else None,
        "checksum": checksum.group(1) if checksum else None,
    }


def kernel_commits(dasm, start, size):
    """[(ciclo, desplazamiento)] de las instrucciones retiradas del kernel."""
    commits = []
    with dasm.open(errors="replace") as stream:
        for line in stream:
            match = COMMIT_LINE.match(line)
            if not match:
                continue
            pc = int(match.group(2), 16)
            if start <= pc < start + size:
                commits.append((int(match.group(1)), pc - start))
    return commits


def run(args, out_dir):
    """Compila, simula y analiza; devuelve el informe y lo guarda."""
    out_dir.mkdir(parents=True, exist_ok=True)
    log = out_dir / "build.log"
    report = {
        "ara_root": str(args.ara_root),
        "simulator": str(args.simulator),
        "nr_lanes": args.nr_lanes,
        "vlen": args.vlen,
        "input": args.input,
        "align": args.align,
    }
    if not build(args, log):
        report["error"] = "build failed, see build.log"
        return report
    files = paths(args)
    report["binaries"] = {k: str(v) for k, v in files.items()}

    report["full"] = simulate(
        args.simulator,
        files["ara_full"],
        out_dir / "full",
        False,
        args.timeout,
    )
    report["traced"] = simulate(
        args.simulator,
        files["ara_traced"],
        out_dir / "traced",
        True,
        args.timeout,
    )

    start, size = symbol(files["ara_traced"])
    commits = kernel_commits(
        out_dir / "traced" / "trace_hart_0.dasm", start, size
    )
    report["kernel"] = {"address": start, "bytes": size}
    (out_dir / "traced" / "kernel-commits.json").write_text(
        json.dumps(commits) + "\n"
    )

    fst = out_dir / "traced" / "sim.fst"
    metrics = out_dir / "traced" / "metrics.json"
    result = subprocess.run(
        [
            str(args.trace_python),
            "-I",
            str(ARA_DIR / "ara_trace.py"),
            str(fst),
            str(args.nr_lanes),
            str(metrics),
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        report["error"] = "ara_trace.py failed: " + result.stderr[-2000:]
    if not args.keep_trace:
        for name in ("sim.fst", "sim.fst.hier"):
            (out_dir / "traced" / name).unlink(missing_ok=True)
        # La traza de retirada ya está resumida en kernel-commits.json.
        (out_dir / "traced" / "trace_hart_0.dasm").unlink(missing_ok=True)
        (out_dir / "full" / "trace_hart_0.dasm").unlink(missing_ok=True)

    (out_dir / "ara.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def add_arguments(parser):
    parser.add_argument("--input", default="tiny")
    parser.add_argument("--num-runs", type=int, default=1)
    parser.add_argument("--align", type=int, default=64)
    parser.add_argument("--nr-lanes", type=int, default=2)
    parser.add_argument("--vlen", type=int, default=256)
    parser.add_argument("--ara-root", type=Path, default=DEFAULT_ARA_ROOT)
    parser.add_argument(
        "--simulator",
        type=Path,
        help="Verilator model (default: <ara-root>/hardware/"
        "build_<L>l_v<VLEN>/verilator/Vara_tb_verilator)",
    )
    parser.add_argument(
        "--trace-python",
        type=Path,
        default=Path(shutil.which("python3") or sys.executable),
        help="Python with pywellen and numpy, for ara_trace.py",
    )
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument(
        "--keep-trace", action="store_true", help="Keep sim.fst and dasm"
    )


def resolve(args):
    args.ara_root = args.ara_root.resolve()
    if args.simulator is None:
        args.simulator = (
            args.ara_root
            / "hardware"
            / f"build_{args.nr_lanes}l_v{args.vlen}"
            / "verilator"
            / "Vara_tb_verilator"
        )
    return args


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    add_arguments(parser)
    parser.add_argument("--out", type=Path, required=True)
    args = resolve(parser.parse_args())
    report = run(args, args.out)
    print(
        json.dumps(
            {k: report.get(k) for k in ("full", "traced", "error")}, indent=2
        )
    )
    ok = (
        "error" not in report
        and report["full"]["passed"]
        and report["traced"]["passed"]
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
