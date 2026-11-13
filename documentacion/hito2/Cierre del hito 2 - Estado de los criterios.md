# Cierre del hito 2: estado de los criterios

Paquete P7 del [plan de cierre de la fase 8][plan-f8], 11 de noviembre de
2026, sobre `9d0a55c128`. Recorre los 16 criterios de la
[sección 15.3 del diseño][cierre] y dice, para cada uno, qué evidencia
existe y qué falta. Es un documento de estado: no añade código, pruebas ni
ejecuciones nuevas.

**Actualización del 12 de noviembre de 2026.** La
[regresión automatizada](#regresión-automatizada) añade los ejemplos al
cierre del plan de evolución y los micro-kernels de anchos y de
Mul/Div/resto. Con ella, los criterios 3 y 13 pasan a acreditados. La
regresión encontró un defecto de Minor con el escalar `x0`, corregido el
mismo día (ver [pendientes](#pendientes-por-dependencia)).

**Actualización del 13 de noviembre de 2026.** El
[estudio de conflictos de bancos][conflictos-vrf] ejecuta en la VPU el
kernel original de Ara `run_vector_short_m4` (Pathfinder 7×512, `int32`,
m4) con la geometría de Ara: VLEN 4096 (`--vlen 4096`), 4 lanes y 8
bancos. En cinco configuraciones del VRF y de la ventana, los 512
resultados coinciden con la referencia. Es sólo offload: no hay
comparación de huella con native. Aporta evidencia nueva a los
criterios 5 y 6 y a la [matriz][matriz] (`vmin.vv`, `vslide1up/down.vx`
en m4), sin cambiar su estado.

**Segunda actualización del 13 de noviembre de 2026.** Un bloque corto
aprobado por el usuario cierra los pendientes que preparan las fases 3.3
a 3.5:

- Los criterios 2 y 11 pasan a acreditados, con los casos `dependencies`
  y `widths-retry` de la regresión.
- El criterio 8 gana la ejecución de un fault de memoria (`faults`) y
  sigue parcial por los flags FP y los caminos de rechazo.
- Minor decodifica `vminu.vv` (`minimum`).
- El matiz 4 de observabilidad está corregido (ver
  [pendientes](#pendientes-por-dependencia)).

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
| 2 | Un consumidor no empieza con resultados parciales | Acreditado | [`dependencies`][dependencies]: cadenas FPU → ALU, divisor → ALU, ALU → slide y slide → ALU con resultado verificado. En la traza `VectorTimeline`, ninguna de las 192 dependencias RAW empieza antes de que termine su productor, y los 8 pares independientes de control sí se solapan. También Pathfinder ([fase 8][f8-evidencia]) y la [revisión P5][revision] | — |
| 3 | Suma y memoria unit-stride en 8/16/32/64 y en el perfil LMUL | Acreditado | [`widths`][widths]: `vle`/`vadd.vv`/`vadd.vx`/`vse` en las 22 combinaciones SEW/LMUL del perfil (m1--m8, mf2--mf8 legales), varias tiras y tail, offload y native con huella idéntica | — |
| 4 | `vfadd`/`vfmul`/`vfdiv` FP32 con redondeo y flags al retirar; FPU y ALUs independientes | Parcial | `vfadd.vv` y `vfmul.vv` FP32 en RNE, resultado bit a bit, también con valores que obligan a redondear ([ejemplo al cierre `fp32`][examples]); 384 pares ALU--FPU solapados en la variante `mixed` ([observabilidad][obs-concurrencia]) | `vfdiv.vv`; flags IEEE al retirar; modos de redondeo distintos de RNE; valores especiales |
| 5 | Bancos, puertos, anchuras o ALUs tienen efecto observable y conservan el resultado | Parcial | Bancos, lanes y latencia de red con Pathfinder; ALUs y puertos con el micro-kernel, cuando buffers de clase y entradas por banco dejan de limitar ([observabilidad][obs-recursos]). Con la geometría de Ara, dos puertos por banco bajan el kernel de 13.950 a 12.816 ciclos y dos entradas por operando a 11.952, con el mismo resultado; pasar de 2 a 8 bancos no cambia el tiempo ([conflictos de bancos][conflictos-vrf]) | Anchura de bloque de la VLSU: `max_block_bytes=32` no tuvo efecto porque los comandos ya eran de 32 bytes |
| 6 | Slides en 8/16/32/64 con `vstart`, tail, escalar y solapamientos; red al cruzar lanes | Parcial | `vslide1up/down.vx` en SEW 32, m1, cruzando lanes por la red (744 paquetes remotos en `tiny`, [fase 8][f8-evidencia]); `vslide1down.vx` en SEW 16 con escalar `x0` y tail ([ejemplo al cierre `int16`][examples]); `vslide1up/down.vx` en SEW 32, m4, con 4 lanes y VLEN 4096, sólo offload ([conflictos de bancos][conflictos-vrf]) | SEW 8/64; LMUL m2 y m8; `vstart` ≠ 0; solapamientos legales y rechazo de `vd = vs2` |
| 7 | Colas y recursos con capacidad finita | Acreditado | Diseño de fases 3--7; [revisión P5][revision] (contrapresión con `Stall` y cola acotada); máximos registrados dentro de capacidad (un paquete de memoria, `sequencer.peakOutstanding` ≤ `max_active_commands`) | — |
| 8 | Identidad, retirada, faults, flags y drain preservan sus contratos | Parcial | [Revisión P5][revision]: identidad, orden de retirada, rechazo joven, interrupciones y drain sin defectos; todas las ejecuciones retiran lo despachado. [`faults`][faults]: fault de página a mitad de `vle32.v` y de `vse32.v`; Minor recibe la dirección del primer elemento sin mapear y escribe `vstart` con su índice antes de invocar el fault (probado con 1, 4 y 7 elementos mapeados) | Flags FP y caminos de rechazo hasta `IllegalInstFault`; reanudar desde `vstart` (requiere FS, sin manejador de traps en SE) |
| 9 | Matriz actualizada y procedimiento para la siguiente instrucción | Acreditado | [Matriz vigente][matriz] y [plantilla de ficha][plantilla] (P6) | — |
| 10 | Agrupación unit-stride en los cuatro anchos, con fronteras y recorrido por elemento | Parcial | Bloques recortados por línea en Pathfinder ([observabilidad][obs-tiempo]); [`widths`][widths]: los cuatro anchos con arrays que cruzan página y línea y bytes de guarda intactos (3 632 bloques para 2 364 comandos) | Recorrido por elemento: bloques de un elemento partido (`splitElementBlocks` = 0), que requiere elementos desalineados |
| 11 | Cada bloque completa antes del siguiente; datos sobreviven a retry; un paquete en vuelo | Acreditado | Máximo de una petición de memoria en curso en Pathfinder ([fase 8][f8-evidencia]). `widths-retry`: [`widths`][widths] con la memoria limitada a 200 MiB/s (`--mem-bandwidth`) da el mismo resultado que native con 5 246 rechazos del puerto de la VPU (`memory.portRetries`) | — |
| 12 | Estadísticas explican la reducción de peticiones y accesos al VRF | Acreditado | [Análisis de observabilidad][observabilidad]: reparto del tiempo, bloques por comando, tabla de intervalos por grupo y correcciones `ce481d8b79` | Comparación con otros anchos (depende del criterio 10) |
| 13 | Mul/Div/resto `.vv` y `.vx` en la ALU en 8/16/32/64, con temporización propia | Acreditado | [`muldiv`][muldiv]: las 10 formas en los cuatro SEW, con divisor cero y desbordamiento según RVV 1.0; perfiles propios y divisor no segmentado ([fase 6][f6]); cambiar `lane.alu.divide_latencies` cambia el tiempo (66,6 M → 51,7 M ticks) sin cambiar el resultado | — |
| 14 | Interconexión independiente de topología, crossbar detrás, métricas comunes | Acreditado | Diseño y código de la [fase 7][f7]: `VectorInterconnect`, crossbar detrás y métricas comunes separadas de las propias del modelo; usado por Pathfinder | No hay un segundo modelo con el que comparar las métricas (no exigido) |
| 15 | Contratos consolidados y entrega al hito 3 | Acreditado (revisión) | [Revisión P5][revision]: `VectorSupport` como autoridad única, identidad y propiedad revisadas; [entrega al hito 3][entrega-h3] (P8) con el mapa de contratos activos, preparados y ausentes | — |
| 16 | Pathfinder con sus 7 tipos RVV, resultado contrastado y toolchain registrado | Acreditado | `tiny` y `small`, offload y native con huella idéntica; RiVEC `32a14ddfb9`, Clang 20.1.8, perfil de 7 tipos comprobado ([fase 8][f8-evidencia]) | — |

Resumen: 11 acreditados (1, 2, 3, 9, 11, 12, 13 y 16 por ejecución; 7, 14
y 15 por revisión) y 5 parciales (4, 5, 6, 8 y 10). El criterio 15 pasó
a acreditado con P8, el 3 y el 13 con la regresión del 12 de noviembre, y
el 2 y el 11 con la del 13 de noviembre. Ninguno queda pendiente de
implementación en la VPU.

## Pendientes por dependencia

- **Defecto de Minor con `x0` (corregido el 12 de noviembre de 2026):**
  en gem5, un operando escalar `x0` tiene como `srcRegIdx` un `RegId()`
  inválido (`src/arch/riscv/isa/operands.isa`), y `buildVectorCommand`
  (`src/cpu/minor/execute.cc`) lo leía con `thread->getReg()`: saltaba una
  aserción del banco de registros y gem5 abortaba. Lo encontró el ejemplo
  al cierre `int16` (`vslide1down.vx v5, v1, zero`). Ahora
  `readScalarSource` devuelve 0 para ese operando, como fijan la ISA base
  (`x0` vale siempre 0) y RVV 1.0, que toma el escalar de `x[rs1]`. Cubre
  los escalares `.vx`, el de las slides y la base de memoria; sólo el de
  las slides tiene ejecución registrada.
- **Decode de `vminu.vv` en Minor (resuelto el 13 de noviembre de
  2026):** `classifyVectorArithOp` (`src/cpu/minor/decode.cc`) lo
  clasifica como `Minimum` sin signo. Antes, `minimum` paraba en offload
  con "Vector instruction is not supported by VPU"; ahora pasa en los
  cuatro SEW con m1 y m4 ([matriz][matriz]).
- **Observabilidad, matiz 4 de la [revisión P5][revision] (corregido el
  13 de noviembre de 2026):** las esperas de la cabeza y la ocupación del
  secuenciador eran intervalos propios que sólo se cerraban en el
  siguiente cambio. Un volcado no contaba el tramo abierto, y al revisar
  el código apareció un segundo defecto: tras un reset de estadísticas,
  el tramo anterior entraba en la región medida. `ModuleStats` admite
  ahora ganchos de volcado y reset (`setIntervalHooks`), y el secuenciador
  parte los intervalos abiertos en cada volcado y los reinicia en cada
  reset. Con volcados cada 50 000 ticks durante el kernel de Ara, antes
  había 10 periodos con 263 000 ticks de espera en 50 000 ticks; ahora
  ninguno, con el mismo total. En las regiones de medida de las cargas de
  la regresión no cambia nada, porque la VPU está vacía en las m5ops.
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

| Caso | Criterios | Resultado (12 nov. 2026) | Resultado (13 nov. 2026) |
| --- | --- | --- | --- |
| `baseline`, `baseline-simple` | Baseline del hito 1 | PASS | PASS |
| `examples-int16` | 3, 6 (ejemplo al cierre, SEW 16) | FAIL antes del arreglo de `x0`; PASS después | PASS |
| `examples-fp32` | 4 (ejemplo al cierre, FP32) | PASS | PASS |
| `widths` | 3, 10 | PASS | PASS |
| `muldiv` | 13 | PASS | PASS |
| `concurrency-int`, `concurrency-mixed` | 1, 4, 5 | PASS | PASS |
| `pathfinder` | 16 | PASS | PASS |
| `minimum` | Decode de `vminu.vv` | — | PASS |
| `dependencies` | 2 | — | PASS |
| `widths-retry` | 11 | — | PASS |
| `faults` | 8 | — | PASS (el `panic` del fault es el resultado esperado) |

Tras el arreglo de `x0`, los 9 casos del 12 de noviembre pasan. El 13 de
noviembre pasan los 13 casos.

La regresión completa tarda unos 25 s con cuatro casos en paralelo
(`-j 4`). `-v` muestra la salida de compilación, gem5 y programa.

## Campaña de validación sugerida

Las dos primeras filas están implementadas en la regresión (`widths` y
`muldiv`), salvo el tiempo del divisor frente a la suma, que no se compara.
También lo están "Productor--consumidor" (`dependencies`) y "Memoria con
retry y faults" (`widths-retry` y `faults`), salvo reanudar desde
`vstart`. El resto es una propuesta para el equipo; no está implementado. Cada
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
[dependencies]: <../../benchmarks/dependencies/README.md>
[faults]: <../../benchmarks/faults/README.md>
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
[conflictos-vrf]: <Pathfinder - conflictos de bancos del VRF.md>
