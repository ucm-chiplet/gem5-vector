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

"""Convierte una entrada de Pathfinder de RiVEC en una cabecera C.

El formato de RiVEC es: una línea con ``ROWS COLS``, ``ROWS * COLS``
enteros de la matriz de costes por filas y, tras una línea en blanco,
``COLS`` enteros con el resultado de referencia. La cabecera define
``PATHFINDER_ROWS`` y ``PATHFINDER_COLS`` y declara ``pathfinderWall`` y
``pathfinderReference`` para que el programa no necesite leer ficheros.
Las definiciones sólo se emiten en la unidad que define
``PATHFINDER_DEFINE_INPUT`` (``src/data.c``): así un único objeto contiene
los datos, compartido por el programa de gem5 y el de Ara.
``PATHFINDER_DATA_ALIGN`` fija, si se define, el alineamiento de ambos.
"""

import argparse
import sys
from pathlib import Path

INT32_MIN = -(2**31)
INT32_MAX = 2**31 - 1
VALUES_PER_LINE = 16


def parse_input(path):
    """Devuelve (rows, cols, wall, reference) tras validar el formato."""
    tokens = path.read_text(encoding="ascii").split()
    if len(tokens) < 2:
        raise ValueError(f"{path}: missing dimensions")
    rows, cols = int(tokens[0]), int(tokens[1])
    if rows < 1 or cols < 1:
        raise ValueError(f"{path}: invalid dimensions {rows}x{cols}")
    values = [int(token) for token in tokens[2:]]
    expected = rows * cols + cols
    if len(values) != expected:
        raise ValueError(
            f"{path}: expected {expected} values, found {len(values)}"
        )
    for value in values:
        if not INT32_MIN <= value <= INT32_MAX:
            raise ValueError(f"{path}: value {value} is not int32")
    return rows, cols, values[: rows * cols], values[rows * cols :]


def format_array(name, values):
    """Formatea un array const int32_t con líneas de longitud acotada."""
    lines = [f"const int32_t {name}[] PATHFINDER_ALIGNED = {{"]
    for start in range(0, len(values), VALUES_PER_LINE):
        chunk = values[start : start + VALUES_PER_LINE]
        lines.append("    " + ", ".join(str(value) for value in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="RiVEC data_*.in file")
    parser.add_argument("output", type=Path, help="Generated C header")
    args = parser.parse_args()

    try:
        rows, cols, wall, reference = parse_input(args.input)
    except ValueError as error:
        print(f"embed_input.py: {error}", file=sys.stderr)
        return 1

    header = "\n".join(
        [
            f"/* Generated from {args.input.name} by embed_input.py. */",
            "#ifndef PATHFINDER_INPUT_H",
            "#define PATHFINDER_INPUT_H",
            "",
            "#include <stdint.h>",
            "",
            f"#define PATHFINDER_ROWS {rows}",
            f"#define PATHFINDER_COLS {cols}",
            "",
            "#ifdef PATHFINDER_DATA_ALIGN",
            "#define PATHFINDER_ALIGNED "
            "__attribute__((aligned(PATHFINDER_DATA_ALIGN)))",
            "#else",
            "#define PATHFINDER_ALIGNED",
            "#endif",
            "",
            "extern const int32_t pathfinderWall[PATHFINDER_ROWS * "
            "PATHFINDER_COLS];",
            "extern const int32_t pathfinderReference[PATHFINDER_COLS];",
            "",
            "#ifdef PATHFINDER_DEFINE_INPUT",
            format_array("pathfinderWall", wall),
            "",
            format_array("pathfinderReference", reference),
            "#endif",
            "",
            "#endif",
            "",
        ]
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="ascii")
    return 0


if __name__ == "__main__":
    sys.exit(main())
