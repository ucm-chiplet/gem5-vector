#!/usr/bin/env python3
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

"""Conflictos de bancos del VRF de la VPU de gem5 (flag VrfArbitration).

Uso:
    analyze_gem5_vrf.py RUN_DIR --dump BINARIO.dump [--task-entries 4]

RUN_DIR es el --outdir de gem5 con trace.out generado con
--debug-flags=VrfArbitration,VectorTimeline,Exec. Escribe
RUN_DIR/analysis/summary.json y RUN_DIR/analysis/conflicts.csv con las
mismas métricas que hardware/vrf_bank_conflicts/analyze_vrf_conflicts.py
de Ara:

- Ventana: de la m5op que reinicia las estadísticas a la que las vuelca
  (el equivalente de hw_cnt_en_o[0] en Ara).
- Cada línea VrfArbitration es un candidato elegible en un ciclo del LRF
  de una lane: entrada, banco, fila, R/W, peticionario, comando y si se
  concedió. Un banco está en conflicto si lo piden dos o más candidatos en
  el mismo ciclo; un ciclo con conflicto cuenta una vez.
- Las entradas siguen el orden de bindRequestPort: por cada contexto de la
  lane {Lhs, Rhs, Destination}, después la VLSU y después la SLDU.
"""

import argparse
import collections
import csv
import json
import re
from pathlib import Path

ARB = re.compile(
    r"^\s*(\d+): \S+: lane=(\d+) cycle=(\d+) in=(\d+) bank=(\d+) row=(\d+) "
    r"([RW]) req=(\w+) cmd=(\d+) reg=(\d+) granted=(\d)"
)
DISPATCH = re.compile(r"^\s*(\d+): \S+: dispatch cmd=(\d+) pc=(0x[0-9a-f]+)")
START = re.compile(r"^\s*(\d+): \S+: start cmd=(\d+)")
COMPLETE = re.compile(r"^\s*(\d+): \S+: complete cmd=(\d+)")
M5OP = re.compile(r"^\s*(\d+): system\.cpu: T0 : \S+ @\S+\s+: M5Op")

# Equivalencia con los peticionarios de Ara (operand_requester.sv).
ARA_NAME = {
    "Lhs": "AluA",
    "Rhs": "AluB",
    "Dst": "wr_ALU",
    "vlsu_W": "wr_LDU",
    "vlsu_R": "StA",
    "slide_R": "SlideAddrGenA",
    "slide_W": "wr_SLDU",
}


def requester(req, kind, index):
    if req == "lane":
        return ("Lhs", "Rhs", "Dst")[index % 3]
    return f"{req}_{kind}"


def disassembly(path):
    text = {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)$", line)
        if m:
            text[int(m.group(1), 16)] = f"{m.group(2)} {m.group(3)}".strip()
    return text


def analyse(run_dir, dump, nr_banks):
    text = disassembly(dump)
    m5 = []
    cmds = {}
    arb = []
    for line in (run_dir / "trace.out").open():
        if "lane=" in line:
            m = ARB.match(line)
            if m:
                arb.append(m.groups())
            continue
        if m := DISPATCH.match(line):
            cmds[int(m.group(2))] = {
                "pc": int(m.group(3), 16),
                "dispatch": int(m.group(1)) // 1000,
            }
        elif m := START.match(line):
            cmds[int(m.group(2))]["start"] = int(m.group(1)) // 1000
        elif m := COMPLETE.match(line):
            cmds[int(m.group(2))]["end"] = int(m.group(1)) // 1000
        elif m := M5OP.match(line):
            m5.append(int(m.group(1)) // 1000)
    first, last = m5[0], m5[1]
    insns = []
    for cid in sorted(cmds):
        c = cmds[cid]
        if first <= c["dispatch"] <= last:
            insns.append(
                {
                    "cmd": cid,
                    "pc": hex(c["pc"]),
                    "insn": text.get(c["pc"], "?"),
                    "dispatch": c["dispatch"],
                    "start": c.get("start"),
                    "end": c.get("end"),
                }
            )
    insn_of = {i["cmd"]: i["insn"] for i in insns}
    index_of = {i["cmd"]: n for n, i in enumerate(insns)}

    lanes = sorted({int(a[1]) for a in arb})
    cycles = last - first + 1
    summary = {
        "window": {
            "signal": "m5op reset_stats .. dump_stats",
            "first_cycle": first,
            "last_cycle": last,
            "cycles": cycles,
        },
        "vector_instructions": len(insns),
        "instructions": insns,
        "lanes": {},
    }
    rows = []
    for lane in lanes:
        per = collections.defaultdict(lambda: collections.defaultdict(list))
        for tick, ln, cyc, inp, bank, row, kind, req, cmd, reg, gnt in arb:
            cyc = int(cyc)
            if int(ln) != lane or not first <= cyc <= last:
                continue
            name = requester(req, kind, int(inp))
            per[cyc][int(bank)].append(
                {
                    "requester": name,
                    "ara": ARA_NAME[name],
                    "kind": kind,
                    "row": int(row),
                    "cmd": int(cmd),
                    "reg": int(reg),
                    "granted": gnt == "1",
                }
            )
        active = collections.Counter()
        conflict_cycles = 0
        kinds = collections.Counter()
        combos_inter = collections.Counter()
        combos_intra = collections.Counter()
        denied = collections.Counter()
        denied_alone = collections.Counter()
        requests = collections.Counter()
        accesses = [0] * nr_banks
        writes = [0] * nr_banks
        multi = 0
        for cyc in range(first, last + 1):
            banks = per.get(cyc, {})
            n_active = 0
            cyc_kinds = set()
            n_conf = 0
            for bank, cands in banks.items():
                for c in cands:
                    requests[c["ara"]] += 1
                    if c["granted"]:
                        n_active += 1
                        accesses[bank] += 1
                        writes[bank] += c["kind"] == "W"
                    elif len(cands) == 1:
                        denied_alone[c["ara"]] += 1
                if len(cands) < 2:
                    continue
                n_conf += 1
                same = len({c["cmd"] for c in cands}) == 1
                kind = "intra" if same else "inter"
                cyc_kinds.add(kind)
                combo = " + ".join(sorted(c["ara"] for c in cands))
                (combos_intra if same else combos_inter)[combo] += 1
                for c in cands:
                    if not c["granted"]:
                        denied[c["ara"]] += 1
                    rows.append(
                        {
                            "lane": lane,
                            "cycle": cyc,
                            "rel_cycle": cyc - first,
                            "bank": bank,
                            "kind": kind,
                            "requester": c["requester"],
                            "ara_equivalent": c["ara"],
                            "access": "write" if c["kind"] == "W" else "read",
                            "cmd": c["cmd"],
                            "insn_index": index_of.get(c["cmd"]),
                            "insn": insn_of.get(c["cmd"], ""),
                            "reg": c["reg"],
                            "bank_row": c["row"],
                            "word_addr": c["row"] * nr_banks + bank,
                            "granted": int(c["granted"]),
                        }
                    )
            active[n_active] += 1
            if n_conf:
                conflict_cycles += 1
                multi += n_conf > 1
                kinds["+".join(sorted(cyc_kinds))] += 1
        summary["lanes"][lane] = {
            "cycles": cycles,
            "conflict_cycles": conflict_cycles,
            "conflict_pct": round(100 * conflict_cycles / cycles, 2),
            "cycles_with_conflict_in_2plus_banks": multi,
            "conflict_cycles_by_kind": dict(kinds),
            "active_banks_histogram": {
                str(k): active[k] for k in range(nr_banks + 1)
            },
            "max_active_banks": max(k for k in active if active[k]),
            "accesses_per_bank": accesses,
            "writes_per_bank": writes,
            "reads_per_bank": [a - w for a, w in zip(accesses, writes)],
            "requests_per_requester": dict(requests),
            "denied_in_conflict": dict(denied),
            "denied_without_competitor": dict(denied_alone),
            "inter_combinations": dict(combos_inter.most_common()),
            "intra_combinations": dict(combos_intra.most_common()),
        }
    out = run_dir / "analysis"
    out.mkdir(exist_ok=True)
    (out / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False)
    )
    with open(out / "conflicts.csv", "w", newline="") as f:
        fields = [
            "lane",
            "cycle",
            "rel_cycle",
            "bank",
            "kind",
            "requester",
            "ara_equivalent",
            "access",
            "cmd",
            "insn_index",
            "insn",
            "reg",
            "bank_row",
            "word_addr",
            "granted",
        ]
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("run_dir", type=Path)
    p.add_argument("--dump", type=Path, required=True)
    p.add_argument("--banks", type=int, default=8)
    a = p.parse_args()
    s = analyse(a.run_dir, a.dump, a.banks)
    print(
        json.dumps(
            {k: v for k, v in s.items() if k != "instructions"},
            indent=1,
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
