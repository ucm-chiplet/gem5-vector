# Multiplicación, división y resto enteros

Micro-kernel del criterio 13 del [hito 2][cierre]: `vmul`, `vdiv`,
`vdivu`, `vrem` y `vremu`, en formas `.vv` y `.vx`, con SEW 8/16/32/64 y
LMUL m1, ejecutados en la ALU de la VPU.

## Kernel

Para cada SEW y cada tira:

- Un bloque `.vv` carga A (`vs2`) y B (`vs1`) y ejecuta las cinco
  operaciones: `vd = vs2 op vs1`.
- Tres bloques `.vx` ejecutan las cinco operaciones sobre A con los
  escalares 0, -1 y -3, pasados en un registro. Con SEW < 64 sólo cuentan
  los bits bajos del escalar.

Los primeros 12 elementos son casos límite: divisor cero, `-2^(SEW-1) / -1`,
combinaciones de signo, máximos y mínimos. El resto son pseudoaleatorios
en todo el rango, con un divisor pequeño uno de cada tres. `ELEMENTS`
(40) supera VLMAX con SEW 8 (32): hay varias tiras y una final parcial.

Resultados que fija RVV 1.0 y reproduce la referencia escalar:

| Caso | `vdivu` | `vdiv` | `vremu` | `vrem` |
| --- | --- | --- | --- | --- |
| Divisor cero | 2^SEW − 1 | −1 | dividendo | dividendo |
| `-2^(SEW-1) / -1` | — | −2^(SEW−1) | — | 0 |

`vmul` conserva los SEW bits bajos del producto.

Referencia: RVV 1.0, edición 20240411, *Vector Single-Width Integer
Multiply Instructions* y *Vector Integer Divide Instructions*.

## Uso

```bash
python3 benchmarks/muldiv/run.py --help
python3 benchmarks/muldiv/run.py                  # ambos modos
python3 benchmarks/muldiv/run.py --elements 100 -v
python3 benchmarks/muldiv/run.py --mode offload \
    --config-arg=--vpu-set=lane.alu.divide_latencies=4,8,16,32
make -C benchmarks/muldiv help
```

`run.py` añade siempre en offload `--vpu-set supported_sews=8,16,32,64`,
porque la configuración por defecto sólo habilita SEW 16 y 32. En offload
exige productos, divisiones y restos en la ALU.

El escalar 0 va en un registro normal y no en `x0`: Minor no captura hoy
`x0` como escalar (ver el [ejemplo al cierre](../examples/README.md)).

[cierre]: <../../documentacion/hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
