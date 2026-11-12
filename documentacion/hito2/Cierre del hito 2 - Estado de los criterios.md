# Cierre del hito 2: estado de los criterios

Paquete P7 del [plan de cierre de la fase 8][plan-f8], 11 de noviembre de
2026, sobre `9d0a55c128`. Recorre los 16 criterios de la
[sección 15.3 del diseño][cierre] y dice, para cada uno, qué evidencia
existe y qué falta. Es un documento de estado: no añade código, pruebas ni
ejecuciones nuevas.

**Actualización del 12 de noviembre de 2026.** La
[regresión automatizada](#regresión-automatizada) añade los ejemplos al
cierre del plan de evolución y los micro-kernels de anchos y de
Mul/Div/resto. Con ella, los criterios 3 y 13 pasan a acreditados y se
encuentra un defecto de Minor con el escalar `x0` (criterio 6).

## Veredicto

**El hito 2 se cierra con pendientes de evidencia.** Todas las capacidades
del diseño están implementadas y compilan. Hay ejecución desde programa
verificada para concurrencia, Pathfinder, el efecto de los recursos, la
observabilidad, suma y memoria en todos los SEW y LMUL del perfil, y
Mul/Div/resto en los cuatro SEW. Los criterios parciales quedan para la
[campaña de validación sugerida][campana], cuyas pruebas define el equipo
(ver "Fuera de alcance" del plan).

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
Minor 2; 4 comandos activos en la VPU) salvo los barridos indicados y los
micro-kernels de anchos y Mul/Div/resto, que habilitan SEW 8/16/32/64 (y
todos los LMUL en el de anchos) con `--vpu-set`.

## Estado por criterio

| # | Criterio (resumen) | Estado | Evidencia | Falta |
| --- | --- | --- | --- | --- |
| 1 | Dos instrucciones independientes progresan a la vez | Acreditado | Micro-kernel de concurrencia: 384 de 512 inicios solapados, hasta 2 ejecutándose (4 con la ventana de Minor en 4); sin solapamiento con `max_active_commands=1`, +5,1 % de tiempo ([observabilidad][obs-concurrencia]) | — |
| 2 | Un consumidor no empieza con resultados parciales | Parcial | Pathfinder es una cadena RAW con resultado verificado ([fase 8][f8-evidencia]); la [revisión P5][revision] confirma inicio en orden con scoreboard RAW/WAR/WAW | Prueba dirigida productor--consumidor con unidades de latencia distinta |
| 3 | Suma y memoria unit-stride en 8/16/32/64 y en el perfil LMUL | Acreditado | [`widths`][widths]: `vle`/`vadd.vv`/`vadd.vx`/`vse` en las 22 combinaciones SEW/LMUL del perfil (m1--m8, mf2--mf8 legales), varias tiras y tail, offload y native con huella idéntica | — |
| 4 | `vfadd`/`vfmul`/`vfdiv` FP32 con redondeo y flags al retirar; FPU y ALUs independientes | Parcial | `vfadd.vv` y `vfmul.vv` FP32 en RNE, resultado bit a bit, también con valores que obligan a redondear ([ejemplo al cierre `fp32`][examples]); 384 pares ALU--FPU solapados en la variante `mixed` ([observabilidad][obs-concurrencia]) | `vfdiv.vv`; flags IEEE al retirar; modos de redondeo distintos de RNE; valores especiales |
| 5 | Bancos, puertos, anchuras o ALUs tienen efecto observable y conservan el resultado | Parcial | Bancos, lanes y latencia de red con Pathfinder; ALUs y puertos con el micro-kernel, cuando buffers de clase y entradas por banco dejan de limitar ([observabilidad][obs-recursos]) | Anchura de bloque de la VLSU: `max_block_bytes=32` no tuvo efecto porque los comandos ya eran de 32 bytes |
| 6 | Slides en 8/16/32/64 con `vstart`, tail, escalar y solapamientos; red al cruzar lanes | Parcial | `vslide1up/down.vx` en SEW 32, m1, cruzando lanes por la red (744 paquetes remotos en `tiny`, [fase 8][f8-evidencia]) | **Defecto de Minor:** con el escalar `x0` gem5 aborta ([ejemplo al cierre `int16`][examples]; ver [pendientes](#pendientes-por-dependencia)). SEW 8/16/64; `vstart` ≠ 0; solapamientos legales y rechazo de `vd = vs2` |
| 7 | Colas y recursos con capacidad finita | Acreditado | Diseño de fases 3--7; [revisión P5][revision] (contrapresión con `Stall` y cola acotada); máximos registrados dentro de capacidad (un paquete de memoria, `sequencer.peakOutstanding` ≤ `max_active_commands`) | — |
| 8 | Identidad, retirada, faults, flags y drain preservan sus contratos | Parcial | [Revisión P5][revision]: identidad, orden de retirada, rechazo joven, interrupciones y drain sin defectos; todas las ejecuciones retiran lo despachado | Ninguna ejecución de un fault de memoria, de flags FP ni de un camino de rechazo hasta `IllegalInstFault` |
| 9 | Matriz actualizada y procedimiento para la siguiente instrucción | Acreditado | [Matriz vigente][matriz] y [plantilla de ficha][plantilla] (P6) | — |
| 10 | Agrupación unit-stride en los cuatro anchos, con fronteras y recorrido por elemento | Parcial | Bloques recortados por línea en Pathfinder ([observabilidad][obs-tiempo]); [`widths`][widths]: los cuatro anchos con arrays que cruzan página y línea y bytes de guarda intactos (3 632 bloques para 2 364 comandos) | Recorrido por elemento: bloques de un elemento partido (`splitElementBlocks` = 0), que requiere elementos desalineados |
| 11 | Cada bloque completa antes del siguiente; datos sobreviven a retry; un paquete en vuelo | Parcial | Máximo de una petición de memoria en curso en Pathfinder ([fase 8][f8-evidencia]) | Ningún retry de memoria forzado |
| 12 | Estadísticas explican la reducción de peticiones y accesos al VRF | Acreditado | [Análisis de observabilidad][observabilidad]: reparto del tiempo, bloques por comando, tabla de intervalos por grupo y correcciones `ce481d8b79` | Comparación con otros anchos (depende del criterio 10) |
| 13 | Mul/Div/resto `.vv` y `.vx` en la ALU en 8/16/32/64, con temporización propia | Acreditado | [`muldiv`][muldiv]: las 10 formas en los cuatro SEW, con divisor cero y desbordamiento según RVV 1.0; perfiles propios y divisor no segmentado ([fase 6][f6]); cambiar `lane.alu.divide_latencies` cambia el tiempo (66,6 M → 51,7 M ticks) sin cambiar el resultado | — |
| 14 | Interconexión independiente de topología, crossbar detrás, métricas comunes | Acreditado | Diseño y código de la [fase 7][f7]: `VectorInterconnect`, crossbar detrás y métricas comunes separadas de las propias del modelo; usado por Pathfinder | No hay un segundo modelo con el que comparar las métricas (no exigido) |
| 15 | Contratos consolidados y entrega al hito 3 | Acreditado (revisión) | [Revisión P5][revision]: `VectorSupport` como autoridad única, identidad y propiedad revisadas; [entrega al hito 3][entrega-h3] (P8) con el mapa de contratos activos, preparados y ausentes | — |
| 16 | Pathfinder con sus 7 tipos RVV, resultado contrastado y toolchain registrado | Acreditado | `tiny` y `small`, offload y native con huella idéntica; RiVEC `32a14ddfb9`, Clang 20.1.8, perfil de 7 tipos comprobado ([fase 8][f8-evidencia]) | — |

Resumen: 9 acreditados (1, 3, 9, 12, 13 y 16 por ejecución; 7, 14 y 15
por revisión) y 7 parciales (2, 4, 5, 6, 8, 10 y 11). El criterio 15 pasó
a acreditado con P8, y el 3 y el 13 con la regresión del 12 de noviembre.
Ninguno queda pendiente de implementación en la VPU; el criterio 6 tiene
un defecto conocido en Minor.

## Pendientes por dependencia

- **Defecto de Minor con `x0`:** en gem5, un operando escalar `x0` tiene
  como `srcRegIdx` un `RegId()` inválido
  (`src/arch/riscv/isa/operands.isa`). `buildVectorCommand`
  (`src/cpu/minor/execute.cc`) lo lee con `thread->getReg()` y salta una
  aserción del banco de registros: gem5 aborta. Afecta a
  `vslide1up/down.vx` y a las formas `.vx` aritméticas con `x0` (y a la
  base de memoria si fuera `x0`). Lo encontró el ejemplo al cierre
  `int16` (`vslide1down.vx v5, v1, zero`); con el 0 en otro registro el
  mismo programa pasa. Necesita su propia propuesta: RVV 1.0 define `x0`
  como el valor 0.
- **Rama de Minor:** decode de `vminu.vv`, sin el cual `vminu` no es
  alcanzable desde un programa ([matriz][matriz]).
- **Observabilidad:** matiz 4 de la [revisión P5][revision]: un intervalo
  abierto del secuenciador al volcar estadísticas se cuenta en el periodo
  siguiente, y un volcado de región de medida puede perder la cola final.
  Necesita su propia propuesta.
- **Ventanas:** con la ventana de Minor en 2 por defecto, los 4 comandos
  activos de la VPU no se alcanzan (matiz 5); hay que subir
  `--minor-vector-window` para estudiarlos.

## Regresión automatizada

[`benchmarks/run_hito2.py`][regresion] ejecuta todas las cargas en offload
y native y devuelve 0 sólo si todas pasan: compilación con el perfil RVV
esperado, `PASS` con verificación escalar en cada modo, huellas iguales y,
en offload, trabajo real de la VPU en la unidad esperada. Aprobada por el
usuario el 12 de noviembre de 2026; sus casos quedan abiertos a la
revisión del equipo.

| Caso | Criterios | Resultado (12 nov. 2026) |
| --- | --- | --- |
| `baseline`, `baseline-simple` | Baseline del hito 1 | PASS |
| `examples-int16` | 3, 6 (ejemplo al cierre, SEW 16) | **FAIL**: defecto de Minor con `x0` |
| `examples-fp32` | 4 (ejemplo al cierre, FP32) | PASS |
| `widths` | 3, 10 | PASS |
| `muldiv` | 13 | PASS |
| `concurrency-int`, `concurrency-mixed` | 1, 4, 5 | PASS |
| `pathfinder` | 16 | PASS |

La regresión completa tarda unos 20 s con cuatro casos en paralelo
(`-j 4`). `-v` muestra la salida de compilación, gem5 y programa.

## Campaña de validación sugerida

Las dos primeras filas están implementadas en la regresión (`widths` y
`muldiv`), salvo el tiempo del divisor frente a la suma, que no se compara.
El resto es una propuesta para el equipo; no está implementado. Cada
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
[regresion]: <../../benchmarks/README.md>
[examples]: <../../benchmarks/examples/README.md>
[widths]: <../../benchmarks/widths/README.md>
[muldiv]: <../../benchmarks/muldiv/README.md>
[entrega-h3]: <Entrega al hito 3 - Contratos y campaña.md>
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
