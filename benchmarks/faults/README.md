# Fault de página a mitad de un acceso unit-stride

Micro-kernel del criterio 8 del [hito 2][cierre]: un fault de memoria en
la VPU llega a Minor con su dirección y con `vstart` igual al índice del
primer elemento que falla. SEW 32, m1.

## Programa

1. Mapea una página anónima en `0x40000000` con `mmap` (`MAP_FIXED`); la
   página siguiente queda sin mapear.
2. Ejecuta, con `vl` = 8, un `vle32.v` (`VARIANT=load`) o un `vse32.v`
   (`VARIANT=store`) que empieza `INSIDE` elementos antes del final de la
   página. Los elementos `0..INSIDE-1` están mapeados y el `INSIDE` es el
   primero de la página sin mapear.
3. Si el acceso no falla, imprime `UNEXPECTED` y devuelve 1.

Referencia: RVV 1.0, edición 20240411, *Vector Memory Instructions* y
*Synchronous exception behavior*: la excepción se toma en el primer
elemento que falla y `vstart` guarda su índice.

## Resultado esperado

En modo SE gem5 no tiene manejador de traps: al invocar el fault la
simulación termina con un `panic`. [`run.py`](run.py) da PASS si:

- el programa llega al acceso y no continúa;
- gem5 termina con `Page table fault when accessing virtual address
  0x40001000`;
- en offload, Minor traza (flag `MinorExecute`) `VPU Offload returned a
  memory fault: ... vaddr=0x40001000 element=INSIDE vstart=INSIDE`. El
  `vstart` es el que Minor ha escrito antes de invocar el fault.

En native (RVV de Minor) se comprueban los dos primeros puntos, como
contraste del mismo programa sin VPU. La traza completa no se guarda;
`fault-trace.txt` conserva la línea del fault.

Fuera de alcance: reanudar desde `vstart` con un manejador (requiere FS)
y comprobar en memoria los elementos del store anteriores al fault, porque
gem5 se detiene antes.

## Uso

```bash
python3 benchmarks/faults/run.py --help
python3 benchmarks/faults/run.py                   # load y store, ambos modos
python3 benchmarks/faults/run.py --variant store --inside 1 --mode offload
make -C benchmarks/faults help
```

`--inside` acepta de 1 a 7.

[cierre]: <../../documentacion/hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
