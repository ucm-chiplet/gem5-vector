# Anchos y grupos: suma y memoria unit-stride

Micro-kernel de los criterios 3 y 10 del [hito 2][cierre]: la suma entera
y la memoria unit-stride en SEW 8/16/32/64 con todos los LMUL del perfil.

## Kernel

Para cada una de las 22 combinaciones legales (SEW ≤ LMUL · ELEN, con
ELEN = 64), un bucle por tiras ejecuta:

```asm
vsetvli    vl, avl, e<SEW>, <LMUL>, tu, mu
vle<SEW>.v v8,  (A)
vle<SEW>.v v16, (B)
vadd.vv    v24, v8, v16        # O0 = A + B   (mod 2^SEW)
vse<SEW>.v v24, (O0)
vadd.vx    v24, v8, x          # O1 = A + x   (bits bajos de x)
vse<SEW>.v v24, (O1)
```

| SEW | LMUL |
| --- | --- |
| 8 | mf8, mf4, mf2, m1, m2, m4, m8 |
| 16 | mf4, mf2, m1, m2, m4, m8 |
| 32 | mf2, m1, m2, m4, m8 |
| 64 | m1, m2, m4, m8 |

- `v8`, `v16` y `v24` están alineados para cualquier LMUL hasta m8.
- `x` es `0x0123456789abcdef`: con SEW < 64 sólo cuentan sus bits bajos.
- Cada array empieza 24 bytes antes de un límite de página de 4 KiB: la
  primera tira cruza página y líneas.
- `ELEMENTS` (300) supera el mayor VLMAX (256, SEW 8 y m8) y no es
  múltiplo de ningún VLMAX: hay varias tiras y una final parcial.
- Tras cada salida hay 64 bytes de guarda que no deben cambiar.
- La verificación es escalar, módulo 2^SEW, sobre todos los elementos.

Referencia: RVV 1.0, edición 20240411, *Vector Single-Width Integer Add
and Subtract*, *Vector Unit-Stride Instructions*, *Vector Register
Grouping* y *Configuration-Setting Instructions*.

## Uso

```bash
python3 benchmarks/widths/run.py --help
python3 benchmarks/widths/run.py                  # ambos modos
python3 benchmarks/widths/run.py --elements 500 -v
make -C benchmarks/widths help
```

La configuración por defecto de `riscv_vpu.py` sólo habilita SEW 16/32 y
m1. `run.py` añade siempre en offload `--vpu-set
supported_sews=8,16,32,64` y `--vpu-set supported_lmuls=-3,-2,-1,0,1,2,3`.
En offload exige trabajo de suma en la ALU y bloques en la VLSU.

### Retries de memoria (criterio 11)

El caso `widths-retry` de la regresión ejecuta este kernel con la
memoria limitada a 200 MiB/s, para que `SimpleMemory` rechace peticiones
mientras está ocupada, y exige que la VPU haya recibido rechazos de su
puerto:

```bash
python3 benchmarks/widths/run.py --mode offload \
    --config-arg=--mem-bandwidth=200MiB/s \
    --require-nonzero=memory.portRetries
```

El resultado debe ser el mismo que sin límite (la huella se compara con
native). El 13 de noviembre de 2026 hubo 5 246 rechazos en la región del
kernel; sin el límite hay 704, por la ocupación del bus de memoria.

## Fuera de este kernel

- Elementos desalineados que crucen una línea (recorrido por elemento,
  `vlsu.splitElementBlocks`): RVV permite que un acceso desalineado
  falle, así que necesita un caso propio.
- Otras operaciones con LMUL distinto de m1.

[cierre]: <../../documentacion/hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
