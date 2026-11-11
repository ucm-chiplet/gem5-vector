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

"""Comprueba el perfil RVV de un benchmark de la VPU.

Lee el desensamblado de ``llvm-objdump -d --mattr=+v`` y exige que:

1. Sólo aparezcan los tipos RVV indicados con ``--types``.
2. Aparezcan todos, para detectar que el compilador no haya sustituido
   alguno.
3. Todas las instrucciones RVV pertenezcan al símbolo del kernel
   (``--kernel``), de modo que la preparación y la verificación sean
   escalares.

En RV64GC ningún mnemónico escalar empieza por ``v``, así que basta el
prefijo para reconocer una instrucción vectorial.
"""

import argparse
import collections
import re
import sys
from pathlib import Path

SYMBOL = re.compile(r"^[0-9a-f]+ <(?P<name>[^>]+)>:$")
# Dirección, bytes de la codificación y mnemónico separados por espacios
# o tabuladores, como imprime llvm-objdump.
INSTRUCTION = re.compile(
    r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s+)+(?P<mnemonic>[a-z][a-z0-9.]*)"
)


def collect(disassembly):
    """Devuelve {símbolo: Counter(mnemónico RVV)} del desensamblado."""
    per_symbol = collections.defaultdict(collections.Counter)
    symbol = None
    for line in disassembly.splitlines():
        header = SYMBOL.match(line)
        if header:
            symbol = header.group("name")
            continue
        instruction = INSTRUCTION.match(line)
        if instruction and instruction.group("mnemonic").startswith("v"):
            per_symbol[symbol][instruction.group("mnemonic")] += 1
    return per_symbol


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disassembly", type=Path)
    parser.add_argument("--kernel", required=True, help="Kernel symbol name")
    parser.add_argument(
        "--types",
        required=True,
        help="Comma-separated RVV mnemonics allowed and required",
    )
    args = parser.parse_args()
    kernel_types = {item for item in args.types.split(",") if item}

    per_symbol = collect(args.disassembly.read_text(encoding="utf-8"))
    found = collections.Counter()
    for counts in per_symbol.values():
        found.update(counts)

    errors = []
    unexpected = sorted(set(found) - kernel_types)
    if unexpected:
        errors.append("unexpected RVV types: " + ", ".join(unexpected))
    missing = sorted(kernel_types - set(found))
    if missing:
        errors.append("missing kernel types: " + ", ".join(missing))
    outside = sorted(name for name in per_symbol if name != args.kernel)
    if outside:
        errors.append("RVV outside the kernel in: " + ", ".join(outside))

    for mnemonic in sorted(found):
        print(f"{mnemonic:16} {found[mnemonic]}")
    if errors:
        for error in errors:
            print(f"check_profile.py: {error}", file=sys.stderr)
        return 1
    print(f"OK: {len(found)} RVV types, all in {args.kernel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
