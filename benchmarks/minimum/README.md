# Mínimo entero con y sin signo

Micro-kernel de `vmin.vv` y `vminu.vv` con SEW 8/16/32/64 y LMUL m1 y m4,
ejecutados en la ALU de la VPU. Acredita el decode de `vminu.vv` en Minor,
pendiente al cierre del [hito 2][cierre-estado].

## Kernel

Para cada LMUL, cada SEW y cada tira, un bloque carga A (`vs2`) y B
(`vs1`) y calcula `vmin.vv` y `vminu.vv`. Los registros son múltiplos de
4, así que el mismo bloque es válido con m1 y m4.

Los primeros 10 elementos son casos en los que ambos resultados difieren
(un operando con el bit de signo activo, `-1` frente a `0`, mínimo
frente a máximo) o coinciden. El resto son pseudoaleatorios en todo el
rango. `ELEMENTS` (150) supera VLMAX con SEW 8 y m4 (128 con VLEN 256):
todas las configuraciones tienen varias tiras y una final parcial.

Referencia: RVV 1.0, edición 20240411, *Vector Integer Min/Max
Instructions*: `vmin` compara en complemento a dos y `vminu` como enteros
sin signo.

## Uso

```bash
python3 benchmarks/minimum/run.py --help
python3 benchmarks/minimum/run.py                  # ambos modos
python3 benchmarks/minimum/run.py --elements 300 -v
make -C benchmarks/minimum help
```

`run.py` añade siempre en offload `--vpu-set supported_sews=8,16,32,64` y
`--vpu-set supported_lmuls=0,2`, porque la configuración por defecto sólo
habilita SEW 16 y 32 y m1. En offload exige mínimos en la ALU
(`minimumElements`).

[cierre-estado]: <../../documentacion/hito2/Cierre del hito 2 - Estado de los criterios.md>
