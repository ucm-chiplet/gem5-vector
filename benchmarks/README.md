# Benchmarks de la VPU

## Regresión de cierre del hito 2

[`run_hito2.py`](run_hito2.py) ejecuta todas las cargas siguientes en
offload (MinorCPU con VPU) y native (RVV de MinorCPU) y devuelve 0 sólo si
todas pasan. Tarda del orden de un minuto con un caso a la vez.

```bash
python3 benchmarks/run_hito2.py --help    # opciones y lista de casos
python3 benchmarks/run_hito2.py           # todos los casos
python3 benchmarks/run_hito2.py -j 4 -v   # en paralelo y con salida detallada
python3 benchmarks/run_hito2.py --only widths --only muldiv
```

Un caso pasa si compila con el perfil RVV esperado, cada modo imprime
`PASS` con su verificación escalar, las huellas de ambos modos coinciden
y, en offload, la VPU completó todo lo despachado e hizo el trabajo en la
unidad esperada. En `faults` el resultado esperado es el `panic` de gem5
con el fault y el `vstart` correctos, porque en SE no hay manejador de
traps. El resultado queda en
`benchmarks/results/hito2-<fecha>/summary.md`.

| Caso | Carga | Qué comprueba |
| --- | --- | --- |
| `baseline`, `baseline-simple` | [Baseline del hito 1](baseline/README.md) | `vadd.vv` y `vadd.vx` con SEW 32 |
| `examples-int16`, `examples-fp32` | [Ejemplos al cierre](examples/README.md) | Los dos fragmentos del plan de evolución, con sus valores |
| `widths` | [Anchos y grupos](widths/README.md) | Criterios 3 y 10: suma y memoria en 22 combinaciones SEW/LMUL |
| `muldiv` | [Mul/Div/resto](muldiv/README.md) | Criterio 13: `.vv` y `.vx` en SEW 8--64, con división por cero y desbordamiento |
| `concurrency-int`, `concurrency-mixed` | [Concurrencia aritmética](concurrency/README.md) | Criterios 1 y 4: comandos a la vez, ALU y FPU juntas |
| `pathfinder` | [Pathfinder](pathfinder/README.md) | Criterio 16: carga RiVEC con sus 7 tipos RVV |
| `minimum` | [Mínimo entero](minimum/README.md) | `vmin.vv` y `vminu.vv` en SEW 8--64, m1 y m4 |
| `dependencies` | [Cadenas productor--consumidor](dependencies/README.md) | Criterio 2: ningún consumidor empieza antes de que termine su productor |
| `widths-retry` | [Anchos y grupos](widths/README.md) con `--mem-bandwidth=200MiB/s` | Criterio 11: el resultado no cambia con la memoria rechazando peticiones; exige `memory.portRetries` > 0 |
| `faults` | [Fault de página](faults/README.md) | Criterio 8: fault a mitad de `vle32`/`vse32`, con dirección y `vstart` |

Cada carga se puede ejecutar también por separado con su `run.py`.

## Uso común

Todos los `run.py` y `sweep.py` aceptan `--help`, que explica sus
opciones con ejemplos, y `-v`/`--verbose`, que muestra mientras se
ejecuta la salida de `make`, de gem5 y del programa y, al final, las
métricas de la región del kernel. Sin `-v` sólo se imprime el resultado;
los registros completos se guardan siempre en `results/`.
`--require-nonzero=STAT` exige además que una estadística de la VPU en
la región del kernel no sea 0 en offload, por ejemplo
`--require-nonzero=memory.portRetries`.

Cada `Makefile` tiene `make help` con sus variables. `make check` compila
y comprueba con [`check_profile.py`](common/check_profile.py) que el
binario contiene exactamente los tipos RVV del kernel, todos dentro de él.

[`common/`](common/): runner y barrido compartidos (`vpu_bench.py`),
comprobación del perfil RVV (`check_profile.py`) y arranque sin libc con
m5ops (`vpu_runtime.h`).

Las fuentes y los scripts se suben a Git. Los binarios y los resultados
permanecen en carpetas locales ignoradas: `*/build/`, `*/results/` y
`results/`. Cada ejecución genera su propia guía `summary.md`.
