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

"""Comprueba que varios binarios ejecutan el mismo kernel que un objeto.

Compara, instrucción a instrucción, los bytes del símbolo del kernel
(``runVector`` por defecto) en el objeto compilado (``kernel.o``) y en cada
binario enlazado (el de gem5 y el de Ara). Sólo pueden diferir los campos
de inmediato de las instrucciones con relocación, que el enlazador
rellena con direcciones distintas en cada plataforma; opcode, registros y
el resto de la codificación deben coincidir. Una relocación de relajación
(``R_RISCV_RELAX``) o de tipo desconocido es un error, porque el enlazador
podría reescribir la instrucción.

Además comprueba que los símbolos de datos indicados con ``--data`` tienen
el mismo desplazamiento respecto a ``--line`` bytes en todos los binarios:
el número de bloques de memoria depende de ese desplazamiento.

Devuelve 0 si todo coincide e imprime un resumen; con ``--json`` lo
escribe también en un fichero.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# Bits del campo de inmediato que rellena cada relocación (formatos
# RISC-V de 32 bits y comprimidos). El resto de la codificación se compara.
IMMEDIATE_MASKS = {
    "R_RISCV_HI20": 0xFFFFF000,
    "R_RISCV_PCREL_HI20": 0xFFFFF000,
    "R_RISCV_LO12_I": 0xFFF00000,
    "R_RISCV_PCREL_LO12_I": 0xFFF00000,
    "R_RISCV_LO12_S": 0xFE000F80,
    "R_RISCV_PCREL_LO12_S": 0xFE000F80,
    "R_RISCV_BRANCH": 0xFE000F80,
    "R_RISCV_JAL": 0xFFFFF000,
    "R_RISCV_RVC_BRANCH": 0x1C7C,
    "R_RISCV_RVC_JUMP": 0x1FFC,
}
# auipc + jalr: dos instrucciones con una sola relocación.
CALL_RELOCATIONS = {"R_RISCV_CALL", "R_RISCV_CALL_PLT"}

SYMBOL_LINE = re.compile(
    r"^\s*\d+:\s+(?P<value>[0-9a-f]+)\s+(?P<size>\d+)\s+\w+\s+\w+\s+\w+"
    r"\s+\S+\s+(?P<name>\S+)$"
)
RELOCATION_LINE = re.compile(
    r"^(?P<offset>[0-9a-f]+)\s+[0-9a-f]+\s+(?P<type>R_RISCV_\w+)\s"
)
HEX_LINE = re.compile(
    r"^\s*(?P<address>[0-9a-f]+)\s+(?P<words>(?:[0-9a-f]{1,8}\s?)+)"
)


def run(tool, *arguments):
    return subprocess.run(
        [tool, *arguments], check=True, capture_output=True, text=True
    ).stdout


def symbols(readelf, path):
    """Devuelve {nombre: (valor, tamaño)} de la tabla de símbolos."""
    result = {}
    for line in run(readelf, "-s", "-W", str(path)).splitlines():
        match = SYMBOL_LINE.match(line)
        if match:
            result[match.group("name")] = (
                int(match.group("value"), 16),
                int(match.group("size")),
            )
    return result


def symbol_bytes(objdump, path, start, size):
    """Bytes de [start, start + size) de .text, leídos con objdump -s.

    objdump -s vuelca la sección entera (no respeta --start-address), así
    que se reconstruye por direcciones y se recorta aquí.
    """
    text = run(objdump, "-s", "-j", ".text", str(path))
    base = None
    data = bytearray()
    for line in text.splitlines():
        match = HEX_LINE.match(line)
        if not match:
            continue
        address = int(match.group("address"), 16)
        if base is None:
            base = address
        elif address != base + len(data):
            raise ValueError(
                f"{path}: discontinuous .text dump at {address:#x}"
            )
        # Formato: dirección, hasta 4 grupos de 8 dígitos y el texto ASCII.
        for group in line.split()[1:5]:
            if not re.fullmatch(r"(?:[0-9a-f]{2}){1,4}", group):
                break
            data += bytes.fromhex(group)
    if base is None or start < base or start + size > base + len(data):
        raise ValueError(f"{path}: [{start:#x}, +{size}) outside .text")
    return bytes(data[start - base : start - base + size])


def relocations(readelf, path, start, size):
    """Lista [(offset relativo, tipo)] de .rela.text dentro del símbolo."""
    result = []
    section = None
    for line in run(readelf, "-r", "-W", str(path)).splitlines():
        if line.startswith("Relocation section"):
            section = line.split("'")[1]
            continue
        match = RELOCATION_LINE.match(line)
        if match and section == ".rela.text":
            offset = int(match.group("offset"), 16)
            if start <= offset < start + size:
                result.append((offset - start, match.group("type")))
    return result


def instructions(code):
    """Divide los bytes en instrucciones: [(offset, longitud, valor)]."""
    result = []
    offset = 0
    while offset < len(code):
        low = int.from_bytes(code[offset : offset + 2], "little")
        length = 4 if low & 0x3 == 0x3 else 2
        value = int.from_bytes(code[offset : offset + length], "little")
        result.append((offset, length, value))
        offset += length
    return result


def masks_for(relocs):
    """{offset: máscara de bits que pueden diferir}; error si no se sabe."""
    masks = {}
    errors = []
    for offset, kind in relocs:
        if kind in IMMEDIATE_MASKS:
            masks[offset] = masks.get(offset, 0) | IMMEDIATE_MASKS[kind]
        elif kind in CALL_RELOCATIONS:
            masks[offset] = masks.get(offset, 0) | 0xFFFFF000
            masks[offset + 4] = masks.get(offset + 4, 0) | 0xFFF00000
        else:
            errors.append(f"relocation {kind} at +{offset:#x} not allowed")
    return masks, errors


def compare(reference, other, masks):
    """Compara instrucción a instrucción; devuelve la lista de errores."""
    errors = []
    if len(reference) != len(other):
        return [f"{len(other)} instructions, expected {len(reference)}"]
    for (offset, length, value), (o_offset, o_length, o_value) in zip(
        reference, other
    ):
        if (offset, length) != (o_offset, o_length):
            errors.append(f"instruction boundary differs at +{offset:#x}")
            continue
        keep = ~masks.get(offset, 0) & ((1 << (8 * length)) - 1)
        if value & keep != o_value & keep:
            errors.append(
                f"+{offset:#x}: {o_value:#0{2 + 2 * length}x} differs from "
                f"{value:#0{2 + 2 * length}x} outside relocated fields"
            )
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object", type=Path, help="Kernel object (kernel.o)")
    parser.add_argument("binaries", type=Path, nargs="+")
    parser.add_argument("--symbol", default="runVector")
    parser.add_argument(
        "--data",
        default="vectorResult,pathfinderWall",
        help="Comma-separated data symbols whose line offset must match",
    )
    parser.add_argument("--line", type=int, default=64)
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument("--readelf", default="llvm-readelf")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    report = {"symbol": args.symbol, "object": str(args.object)}
    errors = []

    object_symbols = symbols(args.readelf, args.object)
    if args.symbol not in object_symbols:
        print(f"compare_kernel.py: {args.symbol} not in {args.object}")
        return 1
    start, size = object_symbols[args.symbol]
    reference = instructions(
        symbol_bytes(args.objdump, args.object, start, size)
    )
    masks, reloc_errors = masks_for(
        relocations(args.readelf, args.object, start, size)
    )
    errors += reloc_errors
    report["bytes"] = size
    report["instructions"] = len(reference)
    report["relocated_instructions"] = len(masks)

    data_names = [name for name in args.data.split(",") if name]
    report["binaries"] = {}
    offsets = {}
    for binary in args.binaries:
        table = symbols(args.readelf, binary)
        entry = {}
        if args.symbol not in table:
            errors.append(f"{binary}: {args.symbol} missing")
            continue
        address, bin_size = table[args.symbol]
        entry["address"] = f"{address:#x}"
        if bin_size != size:
            errors.append(f"{binary}: {args.symbol} has {bin_size} bytes")
        else:
            other = instructions(
                symbol_bytes(args.objdump, binary, address, bin_size)
            )
            errors += [
                f"{binary}: {e}" for e in compare(reference, other, masks)
            ]
        entry["data_line_offsets"] = {}
        for name in data_names:
            if name not in table:
                errors.append(f"{binary}: data symbol {name} missing")
                continue
            offset = table[name][0] % args.line
            entry["data_line_offsets"][name] = offset
            offsets.setdefault(name, set()).add(offset)
        report["binaries"][str(binary)] = entry

    for name, values in offsets.items():
        if len(values) > 1:
            errors.append(
                f"{name} has different offsets in {args.line}-byte lines: "
                + ", ".join(str(v) for v in sorted(values))
            )

    report["errors"] = errors
    report["passed"] = not errors
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    for binary, entry in report["binaries"].items():
        print(
            f"{binary}: {args.symbol} at {entry['address']}, data offsets "
            f"{entry['data_line_offsets']}"
        )
    if errors:
        for error in errors:
            print(f"compare_kernel.py: {error}", file=sys.stderr)
        return 1
    print(
        f"OK: {args.symbol} identical in {len(args.binaries)} binaries "
        f"({len(reference)} instructions, {len(masks)} relocated)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
