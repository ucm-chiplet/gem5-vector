# Ejemplos al cierre del hito 2

Programa que ejecuta, instrucción a instrucción, los dos fragmentos de
[Ejemplos al cierre: enteros de 16 bits y FP32][ejemplos] del plan de
evolución, y comprueba los valores que da el documento.

| Variante | Instrucciones | Caso del documento |
| --- | --- | --- |
| `int16` (por defecto) | `vsetvli e16, m1, tu, mu`, `vle16.v`, `vadd.vv`, `vmul.vv`, `vslide1down.vx` con `zero`, `vse16.v` | A=[100,200,300], B=[2,3,4] → S=[102,203,304], P=[200,600,1200], D=[200,300,0] |
| `fp32` | `vsetvli e32, m1, tu, mu`, `vle32.v`, `vfmul.vv`, `vfadd.vv`, `vse32.v` | A=[1,2,3], B=[0.5,1.5,2] → P=[0.5,3,6], S=[1.5,3.5,5] |

Cada variante ejecuta dos casos en `runKernel()`:

1. **El del documento**, 3 elementos, contra los valores exactos escritos.
2. **`ELEMENTS` elementos pseudoaleatorios** (100 por defecto): varias
   tiras, una tira final parcial y slides que cruzan lanes. En `int16`
   los valores cubren todo el rango de 16 bits, así que sumas y productos
   desbordan (módulo 2^16). En `fp32` son cocientes por 7, que obligan a
   redondear; se comparan bit a bit con las instrucciones escalares F en
   RNE.

El documento muestra un solo bloque de `t0` elementos; aquí se repite en
un bucle por tiras. Cada tira es una `vslide1down`, así que el último
elemento de cada tira recibe el escalar 0 y no el primero de la siguiente.
La referencia reproduce ese comportamiento con los `vl` registrados.

Referencia: RVV 1.0, edición 20240411, *Vector Single-Width Integer Add
and Subtract*, *Vector Single-Width Integer Multiply Instructions*,
*Vector Slide1down*, *Vector Single-Width Floating-Point Add/Subtract* y
*Multiply/Divide Instructions*.

## Uso

```bash
python3 benchmarks/examples/run.py --help
python3 benchmarks/examples/run.py                     # int16, ambos modos
python3 benchmarks/examples/run.py --variant fp32 -v   # salida detallada
make -C benchmarks/examples help
```

Usa la configuración por defecto de `riscv_vpu.py`, que ya habilita SEW
16 y 32 con m1. En offload exige que la ALU (suma y producto) y la
SlideUnit, o la FPU, hayan hecho trabajo.

## Estado

Ambas variantes pasan en offload y native. `int16` destapó un defecto de
Minor: leía el escalar `x0` de `vslide1down.vx` como un registro inválido
(`RegId()` en gem5) y gem5 abortaba. Se corrigió el 12 de noviembre de
2026; ver el [cierre del hito 2][cierre].

[ejemplos]: <../../documentacion/Plan de evolución - Hitos 2 a 5.md#ejemplos-al-cierre-enteros-de-16-bits-y-fp32>
[cierre]: <../../documentacion/hito2/Cierre del hito 2 - Estado de los criterios.md>
