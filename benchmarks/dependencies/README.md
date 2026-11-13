# Cadenas productor--consumidor

Micro-kernel del criterio 2 del [hito 2][cierre]: un consumidor no
empieza con resultados parciales. Cuatro cadenas RAW entre unidades de
latencia distinta, con SEW 32 y m1:

| Cadena | Productor | Consumidor |
| --- | --- | --- |
| FPU → ALU | `vfmul.vv v3, v13, v14` | `vadd.vv v4, v3, v13` |
| Divisor → ALU | `vdiv.vv v5, v1, v2` | `vadd.vv v6, v5, v1` |
| ALU → slide | `vadd.vv v7, v1, v2` | `vslide1down.vx v8, v7, x` |
| Slide → ALU | `vslide1down.vx v9, v1, x` | `vmin.vv v10, v9, v2` |
| Control (independiente) | `vdiv.vv v11, v1, v2` | `vadd.vv v12, v1, v2` |

`vadd.vv v4` suma como enteros los bits del producto FP32 y del operando
FP32: un registro vectorial no tiene tipo. Los divisores nunca son 0.
Con 60 elementos y VLMAX 8 hay 8 tiras, la última parcial.

## Comprobación

En offload, `run.py` activa la traza `VectorTimeline` (despacho, inicio en
el secuenciador y fin de cada comando) y sube la ventana de Minor a 4
(`--minor-vector-window=4`), para que el consumidor espere en la VPU y no
en Minor. [`check_timeline.py`](check_timeline.py):

- Para cada comando busca, por cada registro fuente, el último comando
  anterior que lo escribió, y exige que el consumidor se inicie en el
  mismo tick o después de que el productor complete. Comprueba todas las
  dependencias RAW, también las de cargas y stores, y exige que aparezcan
  las cuatro cadenas de la tabla.
- Exige que alguna `vadd.vv` de control se inicie antes de que termine
  su `vdiv.vv`. Así se comprueba que la traza muestra solapamiento
  cuando las dependencias lo permiten: sin él, no ver solapamiento en las
  cadenas no probaría nada.

El resultado queda en `results/<fecha>/offload/timeline-check.txt`. El
script también se puede usar por separado:

```bash
python3 benchmarks/dependencies/check_timeline.py RUN_DIR BINARIO.disasm
```

Las slides no entran en la ventana de Minor (matiz 2 de la
[revisión de extremo a extremo][revision]): en las cadenas con slide,
Minor también serializa. La comprobación sigue siendo válida, porque mide
el inicio en el secuenciador.

## Uso

```bash
python3 benchmarks/dependencies/run.py --help
python3 benchmarks/dependencies/run.py             # ambos modos
python3 benchmarks/dependencies/run.py --elements 100 -v
```

En offload exige además productos FP32, divisiones, mínimos y slides en
la VPU.

[cierre]: <../../documentacion/hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[revision]: <../../documentacion/hito2/Revisión de extremo a extremo - Minor y VPU.md>
