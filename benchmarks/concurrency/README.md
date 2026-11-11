# Micro-kernel de concurrencia aritmética para la VPU

Carga sintética para observar varios comandos **ejecutándose a la vez** en
la VPU (criterio 1 del [hito 2][cierre]) y el efecto de ALUs, puertos y
buffers (criterio 5). Pathfinder no sirve para esto: su kernel es una
cadena de dependencias ([análisis de observabilidad][observabilidad]).

## Kernel

Cada tira de `vl` elementos (SEW=32, LMUL=m1) carga sus fuentes, ejecuta
cuatro instrucciones aritméticas consecutivas e independientes y guarda
los cuatro resultados:

| Salida | `integer` (por defecto) | `mixed` |
| --- | --- | --- |
| `o0` | `vadd.vv` a + b | `vadd.vv` a + b |
| `o1` | `vmul.vv` c · d | `vfadd.vv` e + f (FP32) |
| `o2` | `vmin.vv` min(a, d) | `vmin.vv` min(a, d) |
| `o3` | `vadd.vv` b + c | `vfmul.vv` e · f (FP32) |

- El kernel está en ensamblador en línea con registros fijos (`v1`--`v6`
  fuentes, `v8`--`v11` resultados). Así el compilador no reutiliza una
  fuente como destino, lo que añadiría un WAR y serializaría las
  instrucciones.
- En el hito 2 la memoria vectorial es una barrera: los loads y stores no
  se solapan; sólo las cuatro operaciones aritméticas pueden hacerlo.
- Los datos se generan con un LCG escalar: enteros en [−1000, 999] y
  floats múltiplos de 1/8 en [−125, 125), exactos. La verificación es
  escalar sobre todos los elementos: suma y producto enteros módulo 2^32,
  mínimo con signo y FP32 bit a bit con las instrucciones escalares F en
  el mismo modo de redondeo (RNE).
- Los arrays están alineados a 64 bytes, de modo que ningún acceso de
  32 bytes cruza una línea.
- `runKernel()` es `noinline` y está rodeado por las m5ops de
  [`vpu_runtime.h`](../common/vpu_runtime.h), que también da el arranque
  sin libc y la salida por `write`.

Referencia: RVV 1.0, edición 20240411, *Vector Single-Width Integer Add
and Subtract*, *Vector Single-Width Integer Multiply Instructions*,
*Vector Integer Min/Max Instructions*, *Vector Single-Width
Floating-Point Add/Subtract Instructions* y *Vector Single-Width
Floating-Point Multiply/Divide Instructions*.

## Compilar

```bash
make -C benchmarks/concurrency check                       # integer, 1024
make -C benchmarks/concurrency VARIANT=mixed ELEMENTS=256 check
```

Mismas opciones que Pathfinder (Clang, `-O2 -fno-vectorize
-fno-slp-vectorize`, sin libc). `check` exige que el binario contenga
exactamente los tipos RVV de la variante, todos en `runKernel`:
`vsetvli`, `vle32.v`, `vse32.v`, `vadd.vv`, `vmul.vv` y `vmin.vv`
(`integer`), o `vsetvli`, `vle32.v`, `vse32.v`, `vadd.vv`, `vmin.vv`,
`vfadd.vv` y `vfmul.vv` (`mixed`).

## Ejecutar y barrer

```bash
python3 benchmarks/concurrency/run.py                      # ambos modos
python3 benchmarks/concurrency/run.py --variant mixed --mode offload
python3 benchmarks/concurrency/sweep.py                    # lista por defecto
python3 benchmarks/concurrency/sweep.py --configs mis-configs.json
```

Usan el runner común [`vpu_bench.py`](../common/vpu_bench.py), con las
mismas reglas que Pathfinder: un modo pasa si el programa devuelve 0 y
escribe `[concurrency] PASS`; en offload la VPU debe completar todo lo
despachado; con dos modos, las huellas deben coincidir. El barrido por
defecto varía ALUs, puertos, bancos, ventana de Minor y comandos activos.

La ejecución simultánea se lee en `sequencer.overlappedStarts`,
`sequencer.peakOutstanding` y `sequencer.multiActiveTicks`. Los resultados
y su interpretación están en el
[análisis de observabilidad][observabilidad].

[cierre]: <../../documentacion/hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[observabilidad]: <../../documentacion/hito2/Observabilidad - análisis con Pathfinder.md>
