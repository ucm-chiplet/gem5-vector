# Cierre del hito 2: estado de los criterios

Paquete P7 del [plan de cierre de la fase 8][plan-f8], 11 de noviembre de
2026, sobre `aefe8eb621`. Recorre los 16 criterios de la
[sección 15.3 del diseño][cierre] y dice, para cada uno, qué evidencia
existe y qué falta. Es un documento de estado: no añade código, pruebas ni
ejecuciones nuevas.

## Veredicto

**El hito 2 se cierra con pendientes de evidencia.** Todas las capacidades
del diseño están implementadas y compilan. Hay ejecución desde programa
verificada para concurrencia, Pathfinder, el efecto de los recursos y la
observabilidad, pero sólo con SEW 32 y LMUL m1. Los criterios parciales
quedan para la [campaña de validación sugerida][campana],
cuyas pruebas define el equipo (ver "Fuera de alcance" del plan).

Este veredicto no convierte la evidencia parcial en soporte comprobado:
la [matriz vigente][matriz] sigue siendo la referencia de qué se ha
ejecutado. Tampoco hace de los pendientes un requisito para abrir el
hito 3; cada uno se cubrirá cuando lo necesite una carga o una propuesta.

## Estados

- **Acreditado:** un programa lo ejecuta con resultado verificado, o, si
  el criterio es de diseño, hay una revisión registrada.
- **Parcial:** hay evidencia, pero sólo para un subconjunto.
- **Implementado sin evidencia:** existe y compila; ningún programa lo
  ha ejecutado.
- **Pendiente:** falta implementación o depende de otra rama.

Toda la ejecución citada usa la configuración por defecto de
`configs/example/riscv_vpu.py` (SEW 16/32 y m1 habilitados; ventana de
Minor 2; 4 comandos activos en la VPU) salvo los barridos indicados.

## Estado por criterio

| # | Criterio (resumen) | Estado | Evidencia | Falta |
| --- | --- | --- | --- | --- |
| 1 | Dos instrucciones independientes progresan a la vez | Acreditado | Micro-kernel de concurrencia: 384 de 512 inicios solapados, hasta 2 ejecutándose (4 con la ventana de Minor en 4); sin solapamiento con `max_active_commands=1`, +5,1 % de tiempo ([observabilidad][obs-concurrencia]) | — |
| 2 | Un consumidor no empieza con resultados parciales | Parcial | Pathfinder es una cadena RAW con resultado verificado ([fase 8][f8-evidencia]); la [revisión P5][revision] confirma inicio en orden con scoreboard RAW/WAR/WAW | Prueba dirigida productor--consumidor con unidades de latencia distinta |
| 3 | Suma y memoria unit-stride en 8/16/32/64 y en el perfil LMUL | Parcial | `vadd.vv/.vx`, `vle32`/`vse32` en SEW 32, m1 (Pathfinder, concurrencia, baseline) | SEW 8/16/64; m2--m8 y mf2--mf8 |
| 4 | `vfadd`/`vfmul`/`vfdiv` FP32 con redondeo y flags al retirar; FPU y ALUs independientes | Parcial | `vfadd.vv` y `vfmul.vv` FP32 en RNE, resultado bit a bit; 384 pares ALU--FPU solapados en la variante `mixed` ([observabilidad][obs-concurrencia]) | `vfdiv.vv`; flags IEEE al retirar; modos de redondeo distintos de RNE; valores especiales |
| 5 | Bancos, puertos, anchuras o ALUs tienen efecto observable y conservan el resultado | Parcial | Bancos, lanes y latencia de red con Pathfinder; ALUs y puertos con el micro-kernel, cuando buffers de clase y entradas por banco dejan de limitar ([observabilidad][obs-recursos]) | Anchura de bloque de la VLSU: `max_block_bytes=32` no tuvo efecto porque los comandos ya eran de 32 bytes |
| 6 | Slides en 8/16/32/64 con `vstart`, tail, escalar y solapamientos; red al cruzar lanes | Parcial | `vslide1up/down.vx` en SEW 32, m1, cruzando lanes por la red (744 paquetes remotos en `tiny`, [fase 8][f8-evidencia]) | SEW 8/16/64; `vstart` ≠ 0; solapamientos legales y rechazo de `vd = vs2` |
| 7 | Colas y recursos con capacidad finita | Acreditado | Diseño de fases 3--7; [revisión P5][revision] (contrapresión con `Stall` y cola acotada); máximos registrados dentro de capacidad (un paquete de memoria, `sequencer.peakOutstanding` ≤ `max_active_commands`) | — |
| 8 | Identidad, retirada, faults, flags y drain preservan sus contratos | Parcial | [Revisión P5][revision]: identidad, orden de retirada, rechazo joven, interrupciones y drain sin defectos; todas las ejecuciones retiran lo despachado | Ninguna ejecución de un fault de memoria, de flags FP ni de un camino de rechazo hasta `IllegalInstFault` |
| 9 | Matriz actualizada y procedimiento para la siguiente instrucción | Acreditado | [Matriz vigente][matriz] y [plantilla de ficha][plantilla] (P6) | — |
| 10 | Agrupación unit-stride en los cuatro anchos, con fronteras y recorrido por elemento | Parcial | Bloques de 32 bits recortados por línea: 1,5 bloques por comando en Pathfinder por datos alineados a 4 bytes ([observabilidad][obs-tiempo]) | Anchos 8/16/64; cruce de página; bloques de un elemento (`splitElementBlocks`) |
| 11 | Cada bloque completa antes del siguiente; datos sobreviven a retry; un paquete en vuelo | Parcial | Máximo de una petición de memoria en curso en Pathfinder ([fase 8][f8-evidencia]) | Ningún retry de memoria forzado |
| 12 | Estadísticas explican la reducción de peticiones y accesos al VRF | Acreditado | [Análisis de observabilidad][observabilidad]: reparto del tiempo, bloques por comando, tabla de intervalos por grupo y correcciones `2ef0927059` | Comparación con otros anchos (depende del criterio 10) |
| 13 | Mul/Div/resto `.vv` y `.vx` en la ALU en 8/16/32/64, con temporización propia | Implementado sin evidencia | `vmul.vv` SEW 32 en el micro-kernel; perfiles temporales propios y divisor no segmentado en la [fase 6][f6] | `vmul.vx`, `vdiv(u)`, `vrem(u)` en ambas formas; otros SEW; divisor cero y desbordamiento |
| 14 | Interconexión independiente de topología, crossbar detrás, métricas comunes | Acreditado | Diseño y código de la [fase 7][f7]: `VectorInterconnect`, crossbar detrás y métricas comunes separadas de las propias del modelo; usado por Pathfinder | No hay un segundo modelo con el que comparar las métricas (no exigido) |
| 15 | Contratos consolidados y entrega al hito 3 | Parcial | [Revisión P5][revision]: `VectorSupport` como autoridad única, identidad y propiedad revisadas | Entrega al hito 3 (paquete P8) |
| 16 | Pathfinder con sus 7 tipos RVV, resultado contrastado y toolchain registrado | Acreditado | `tiny` y `small`, offload y native con huella idéntica; RiVEC `32a14ddfb9`, Clang 20.1.8, perfil de 7 tipos comprobado ([fase 8][f8-evidencia]) | — |

Resumen: 6 acreditados (1, 9, 12 y 16 por ejecución; 7 y 14 por
revisión), 9 parciales (2, 3, 4, 5, 6, 8, 10, 11 y 15) y uno implementado
sin evidencia (13). Ninguno queda pendiente de implementación en la VPU.

## Pendientes por dependencia

- **Rama de Minor:** decode de `vminu.vv`, sin el cual `vminu` no es
  alcanzable desde un programa ([matriz][matriz]).
- **Observabilidad:** matiz 4 de la [revisión P5][revision]: un intervalo
  abierto del secuenciador al volcar estadísticas se cuenta en el periodo
  siguiente, y un volcado de región de medida puede perder la cola final.
  Necesita su propia propuesta.
- **Ventanas:** con la ventana de Minor en 2 por defecto, los 4 comandos
  activos de la VPU no se alcanzan (matiz 5); hay que subir
  `--minor-vector-window` para estudiarlos.

## Campaña de validación sugerida

Propuesta para el equipo; nada de esto está implementado. Cada
micro-kernel seguiría el patrón de `benchmarks/concurrency/` (runner común,
offload y native, verificación escalar completa) y su propia propuesta.

| Micro-kernel | Criterios | Casos |
| --- | --- | --- |
| Anchos y grupos | 3, 10 | `vle`/`vadd.vv/.vx`/`vse` en SEW 8/16/32/64, m1/m2/m4/m8 y mf2/mf4/mf8 legales, `vl < VLMAX`, datos que crucen línea y página; requiere `--vpu-set supported_sews` y `supported_lmuls` |
| Mul/Div/resto | 13 | `.vv` y `.vx` en los cuatro SEW, divisor cero, `INT_MIN / -1`, valores extremos; tiempo del divisor frente a la suma |
| FP32 completo | 4, 8 | `vfdiv.vv`; operaciones que levanten cada flag IEEE; modos de redondeo vía `frm`; NaN, infinitos y subnormales; lectura de `fflags` tras retirar |
| Slides | 6 | Los cuatro SEW, `vstart` ≠ 0, tail, escalares que requieran truncado o extensión, `vd` igual a `vs1`/escalar legal |
| Productor--consumidor | 2 | Cadenas FPU → ALU y ALU → slide con latencias distintas |
| Caminos de rechazo | 8 | `vm=0`, EEW ≠ SEW, `vslide1up` con `vd = vs2`: comprobar `IllegalInstFault` sin efectos previos |
| Memoria con retry y faults | 8, 11 | Puerto de memoria que rechace peticiones; acceso a página no mapeada a mitad de instrucción y `vstart` resultante |

[campana]: #campaña-de-validación-sugerida
[plan-f8]: <Plan de cierre de la fase 8.md>
[cierre]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[matriz]: <Matriz de decode, representacion y soporte.md>
[plantilla]: <Plantilla de ficha de operación.md>
[revision]: <Revisión de extremo a extremo - Minor y VPU.md>
[observabilidad]: <Observabilidad - análisis con Pathfinder.md>
[obs-tiempo]: <Observabilidad - análisis con Pathfinder.md#reparto-del-tiempo-del-kernel>
[obs-recursos]: <Observabilidad - análisis con Pathfinder.md#efecto-de-los-recursos-criterio-5>
[obs-concurrencia]: <Observabilidad - análisis con Pathfinder.md#concurrencia-aritmética-con-el-micro-kernel>
[f6]: <Fase 6 - FPU, operaciones enteras y varias ALUs.md>
[f7]: <Fase 7 - Interconexión y slides.md>
[f8-evidencia]: <Fase 8 - Mínimo entero para Pathfinder.md#evidencia-de-ejecución-de-pathfinder>
