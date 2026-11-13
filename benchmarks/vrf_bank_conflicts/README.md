# Conflictos de bancos del VRF: Pathfinder 7×512 en la VPU

Este benchmark ejecuta en la VPU de gem5 el kernel original de Ara
`run_vector_short_m4` (Pathfinder, 7 filas × 512 columnas, `int32`, LMUL = 4)
y mide los conflictos de bancos del VRF con las mismas definiciones que el
estudio hecho en Ara RTL. El análisis está en
[Pathfinder - conflictos de bancos del VRF][doc].

## Archivos

| Archivo | Función |
| --- | --- |
| `main_gem5.c` | `_start`, m5ops alrededor del kernel y comprobación de los 512 resultados |
| `Makefile` | Enlaza `main_gem5.o` con `kernel/pathfinder.c.o` y `data.S.o` de `ara/apps/pathfinder_r7_c512` (los mismos objetos que se simularon en Ara) y fija el desplazamiento de los datos dentro de la página igual que en Ara |
| `analyze_gem5_vrf.py` | Métricas a partir de `trace.out` (flags `VrfArbitration`, `VectorTimeline` y `Exec`) |
| `results/<escenario>/` | Salida de gem5 y `analysis/summary.json` y `analysis/conflicts.csv` (ignorado por git si `results/` lo está) |

## Requisitos

- Los objetos de Ara: `make bin/pathfinder_r7_c512 config=default` en
  `ara/apps`. Por defecto se buscan en `../../../ara` (`ARA_ROOT`).
- `build/RISCV/gem5.opt` con el flag `VrfArbitration`.

## Escenarios medidos

Todos usan `--vlen 4096 --vpu-set vrf.num_lanes=4 --vpu-set supported_lmuls=0,2
--mem-latency=0ns`.

| Escenario | Opciones adicionales | Ciclos del kernel |
| --- | --- | --- |
| `banks8` | `--vpu-set vrf.banks_per_lane=8` | 13.950 |
| `banks2` | ninguna (2 bancos por lane, el valor predeterminado) | 13.950 |
| `banks8_win8` | `banks8` y cola, comandos activos, contextos y ventana de Minor a 8 | 13.950 |
| `banks8_ports2` | `banks8` y `--vpu-set bank.read_write_ports=2 --vpu-set bank.access_entries=2` | 12.816 |
| `banks8_opnd2` | `banks8` y `--vpu-alu-operand-entries 2 --vpu-alu-result-entries 2` | 11.952 |

Los comandos completos están en el documento.

[doc]: <../../documentacion/hito2/Pathfinder - conflictos de bancos del VRF.md>
