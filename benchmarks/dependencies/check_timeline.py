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

"""Comprueba el orden productor-consumidor en la traza VectorTimeline.

Lee ``trace.out`` (flag ``VectorTimeline``: ``dispatch``, ``start`` y
``complete`` de cada comando) y el desensamblado del binario. Para cada
comando busca, por cada registro fuente, el último comando anterior que
lo escribió (dependencia RAW) y exige que el consumidor se inicie en el
secuenciador en el mismo tick o después de que el productor complete.

Como control, exige también que en algún par independiente
``vdiv.vv`` → ``vadd.vv`` (sin registros en común) la suma se inicie
antes de que termine la división: así se comprueba que la traza sí
muestra solapamiento cuando las dependencias lo permiten.

Uso directo:
    check_timeline.py RUN_DIR BINARIO.disasm
"""

import collections
import re
import sys
from pathlib import Path

DISPATCH = re.compile(r"^\s*(\d+): \S+: dispatch cmd=(\d+) pc=(0x[0-9a-f]+)")
START = re.compile(r"^\s*(\d+): \S+: start cmd=(\d+)")
COMPLETE = re.compile(r"^\s*(\d+): \S+: complete cmd=(\d+)")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ?)+\s+(\S+)\s*(.*)$")
VREG = re.compile(r"\bv(\d+)\b")

# Pares que el kernel debe ejercitar, como (productor, consumidor).
EXPECTED_CHAINS = (
    ("vfmul.vv", "vadd.vv"),
    ("vdiv.vv", "vadd.vv"),
    ("vadd.vv", "vslide1down.vx"),
    ("vslide1down.vx", "vmin.vv"),
)


def disassembly(path):
    """Dirección -> (mnemónico, registros escritos, registros leídos)."""
    insns = {}
    for line in Path(path).read_text().splitlines():
        m = INSN.match(line)
        if not m or not m.group(2).startswith("v"):
            continue
        mnemonic = m.group(2)
        regs = [int(r) for r in VREG.findall(m.group(3))]
        if not regs or mnemonic.startswith("vset"):
            continue
        # Con LMUL m1 cada operando es un solo registro. Un store lee su
        # primer operando (vs3); las demás instrucciones lo escriben.
        if mnemonic.startswith("vs") and not mnemonic.startswith("vsl"):
            writes, reads = set(), set(regs)
        else:
            writes, reads = {regs[0]}, set(regs[1:])
        insns[int(m.group(1), 16)] = (mnemonic, writes, reads)
    return insns


def timeline(trace):
    """Comandos en orden de despacho, con pc, inicio y fin en ticks."""
    cmds = collections.OrderedDict()
    for line in Path(trace).open():
        if m := DISPATCH.match(line):
            cmds[int(m.group(2))] = {"pc": int(m.group(3), 16)}
        elif m := START.match(line):
            cmds[int(m.group(2))]["start"] = int(m.group(1))
        elif m := COMPLETE.match(line):
            cmds[int(m.group(2))]["end"] = int(m.group(1))
    return cmds


def analyse(run_dir, disasm):
    """Devuelve (errores, resumen) del orden de inicio de la traza."""
    insns = disassembly(disasm)
    cmds = timeline(Path(run_dir) / "trace.out")
    errors = []
    chains = collections.Counter()
    checked = 0
    control_overlaps = 0
    control_pairs = 0
    last_writer = {}
    previous = None
    for cid, cmd in cmds.items():
        if "start" not in cmd or "end" not in cmd:
            errors.append(f"comando {cid} sin inicio o fin en la traza")
            continue
        mnemonic, writes, reads = insns.get(cmd["pc"], ("?", set(), set()))
        cmd["insn"] = mnemonic
        producers = {last_writer[r] for r in reads if r in last_writer}
        for pid in producers:
            producer = cmds[pid]
            checked += 1
            chains[(producer["insn"], mnemonic)] += 1
            if cmd["start"] < producer["end"]:
                errors.append(
                    f"{mnemonic} (cmd {cid}) empieza en {cmd['start']} antes "
                    f"de que termine {producer['insn']} (cmd {pid}) en "
                    f"{producer['end']}"
                )
        # Control: vdiv seguida de una vadd que no depende de ella.
        if (
            previous is not None
            and previous[1]["insn"] == "vdiv.vv"
            and mnemonic == "vadd.vv"
            and previous[0] not in producers
        ):
            control_pairs += 1
            if cmd["start"] < previous[1]["end"]:
                control_overlaps += 1
        for r in writes:
            last_writer[r] = cid
        previous = (cid, cmd)

    for chain in EXPECTED_CHAINS:
        if not chains[chain]:
            errors.append(f"no aparece la cadena {chain[0]} -> {chain[1]}")
    if not control_overlaps:
        errors.append(
            "ningún par independiente vdiv -> vadd se solapa: la traza no "
            "muestra solapamiento y la comprobación no es concluyente"
        )
    summary = {
        "commands": len(cmds),
        "raw_pairs_checked": checked,
        "chains": {f"{p} -> {c}": n for (p, c), n in sorted(chains.items())},
        "control_pairs": control_pairs,
        "control_overlaps": control_overlaps,
    }
    return errors, summary


def check(run_dir, binary):
    """Comprobación para Benchmark.offload_run_checks de vpu_bench."""
    errors, summary = analyse(run_dir, Path(str(binary) + ".disasm"))
    lines = [f"{k}: {v}" for k, v in summary.items()]
    (Path(run_dir) / "timeline-check.txt").write_text(
        "\n".join(lines + [f"ERROR: {e}" for e in errors]) + "\n"
    )
    return errors


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    errors, summary = analyse(sys.argv[1], sys.argv[2])
    for key, value in summary.items():
        print(f"{key}: {value}")
    for error in errors:
        print(f"ERROR: {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
