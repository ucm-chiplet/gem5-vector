# Hito 2: observabilidad y efecto de los recursos con Pathfinder

Paquete P4 del [plan de cierre de la fase 8][plan-f8], 11 de noviembre de
2026. Usa [Pathfinder][benchmark] como carga real para:

1. Explicar en qué se va el tiempo del kernel (criterios 7 y 12 de la
   [sección 15.3][cierre]).
2. Revisar si las métricas son claras y si alguna cuenta dos veces.
3. Comprobar que cambiar recursos tiene efectos observables y conserva el
   resultado (criterio 5).

No modifica estadísticas de Minor ni de la VPU: las correcciones que hacen
falta quedan propuestas al final y requieren su propia aprobación.

## Condiciones

| Dato | Valor |
| --- | --- |
| Código | Commit de esta entrega sobre `10b3a199ed`; `gem5.opt` compilado desde `dad5ab6f5f`, sin cambios posteriores en `src/` |
| Carga | Pathfinder `tiny` (32×32) y `small` (128×1024), `NUM_RUNS=1`, modo offload |
| Configuración base | `configs/example/riscv_vpu.py`: VLEN 256, 2 lanes, 2 bancos por lane, 1 ALU por lane, crossbar, `SimpleMemory` sin cachés, CPU a 1 GHz |
| Región medida | Entre `m5_reset_stats` y `m5_dump_stats`, sólo `runVector()` |
| Herramientas | `benchmarks/pathfinder/run.py` y `sweep.py`; opciones `--vpu-set` y `--minor-vector-window` de `riscv_vpu.py` |

Los ticks son del modelo, no una medida calibrada de Ara.

## Reparto del tiempo del kernel

`tiny`, 52 083 ciclos (52 083 000 ticks). Cada fila es la ocupación de un
recurso: las filas se solapan entre sí y no suman el total.

| Recurso (estadística) | Ciclos | % del kernel | Por unidad de trabajo |
| --- | --- | --- | --- |
| Algún comando despachado y sin terminar (`commands.busyTicks`) | 38 327 | 73,6 % | — |
| Algún comando ejecutándose (`sequencer.busyTicks`) | 37 335 | 71,7 % | — |
| VLSU ocupada (`vlsu.busyTicks`) | 28 035 | 53,8 % | 75 por comando de memoria (372) |
| Espera de memoria (`vlsu.memoryTicks`) | 24 873 | 47,8 % | 44,6 por bloque (558) |
| Tareas aritméticas (`distributor.busyTicks`) | 5 208 | 10,0 % | 14 por tarea (372) |
| SlideUnit (`slide.busyTicks`) | 3 100 | 6,0 % | 12,5 por tarea (248) |
| Ninguna instrucción vectorial en la VPU | 13 756 | 26,4 % | — |

Lectura:

- **La memoria domina.** Más de la mitad del kernel es la VLSU, y casi
  todo es la latencia de `SimpleMemory` (unos 45 ciclos por bloque) con un
  único bloque y un único paquete en vuelo, como fija el hito 2.
- **La ejecución es estrictamente serie.** VLSU + aritmética + slides
  (36 343 ciclos) casi igualan el tiempo con algún comando ejecutándose
  (37 335): no hay solapamiento entre unidades. Ver la sección siguiente.
- **El 26 % restante es CPU:** las lecturas escalares de `aux`/`aux2`, el
  control del bucle, `vsetvli` y el despacho.
- La ALU en sí trabaja 1 ciclo por bundle (`alu0.busyTicks` = 744 ciclos
  por lane). Los 14 ciclos por tarea aritmética son sobre todo lectura de
  operandos del VRF, buffers y writeback.
- Los comandos de memoria generan 1,5 bloques de media porque
  `vectorResult` y `pathfinderWall` sólo están alineados a 4 bytes
  (desplazamiento 60 en la línea de 64): la mitad de las tiras de 32 bytes
  cruzan una línea. Es una propiedad del programa, no del modelo.

Con `small` el reparto es análogo (VLSU 61 % de 5 995 385 ciclos).

## Por qué no hay ejecución simultánea

En todas las configuraciones probadas, `sequencer.overlappedStarts` es 0 y
`sequencer.peakOutstanding` es 1: **la VPU nunca ejecuta dos comandos de
Pathfinder a la vez.** El código generado por tira es:

```text
vle32.v        v8, (dst)        # memoria
vle32.v        v9, (wall)       # memoria: barrera tras la anterior
vslide1up.vx   v10, v8, aux     # RAW con v8
vmin.vv        v10, v8, v10     # RAW con v10
vslide1down.vx v8, v8, aux2     # WAR: el vmin anterior lee v8
vmin.vv        v8, v10, v8      # RAW con v10 y v8
vadd.vv        v8, v9, v8       # RAW con v8
vse32.v        v8, (dst)        # RAW con v8; memoria
```

Cada instrucción depende de la anterior. El único par que podría
solaparse sin renombrado, `vslide1down` con el primer `vmin`, lo impide el
WAR sobre `v8`, porque Clang reutiliza el registro. Con las reglas del
hito 2 (memoria como barrera, sin chaining, scoreboard conservador con
WAR/WAW e inicio en orden) el kernel no admite solapamiento. Pathfinder no
sirve, por tanto, como evidencia del criterio 1; hace falta otra carga con
instrucciones aritméticas independientes.

## Efecto de los recursos (criterio 5)

`sweep.py` varía un eje cada vez respecto a la base. Todas las
configuraciones terminan con `PASS` y la misma huella que la base
(`tiny`: `sum=1731 xor=29`; `small`: `sum=188600 xor=46`).

| Configuración | Opción | Δ ticks `tiny` | Δ ticks `small` | Efecto observado |
| --- | --- | --- | --- | --- |
| `banks4` | `vrf.banks_per_lane=4` | −0,2 % | −0,3 % | Espera de VRF −4,8 % |
| `lanes4` | `vrf.num_lanes=4` | −3,2 % | −4,8 % | Espera de VRF −12 %, SlideUnit −20 % |
| `transit4` | `interconnect.transit_latency=4` | +2,8 % | +3,0 % | Transporte de red ×2,5, SlideUnit +44 % |
| `alus2` | `lane.alus_per_lane=2` | 0 | — | Sin efecto: nunca hay dos operaciones aritméticas a la vez |
| `rw-ports2` | `bank.read_write_ports=2` | 0 | — | Sin efecto: la espera de VRF no cambia; hipótesis, a confirmar, que la limita `access_entries=1` por banco y no los puertos |
| `block32` | `vlsu.max_block_bytes=32` | 0 | — | Sin efecto: los comandos ya son de 32 bytes |
| `window4` | `--minor-vector-window=4` | 0 | — | Sin efecto: no hay instrucciones independientes que adelantar |
| `active1` | `max_active_commands=1` | 0 | — | Sin efecto: la VPU ya ejecuta uno cada vez |

Los recursos que Pathfinder usa (bancos, lanes, red) tienen efecto
observable y conservan el resultado. Los que no tienen efecto lo explica
la estructura serie del kernel, no un fallo de configuración:
`config.ini` confirma que cada valor llega a la VPU o a Minor. El
criterio 5 queda acreditado para bancos, lanes y red; para ALUs y
puertos hace falta una carga con concurrencia aritmética.

## Revisión de las métricas

| # | Hallazgo | Comprobación | Consecuencia |
| --- | --- | --- | --- |
| 1 | `commands.peakOutstanding` y `vectorMultiActiveCycles` (Minor) cuentan comandos despachados y sin terminar, incluidos los que esperan en la cola de la VPU. La descripción de Minor dice "active in VPU". | `sequencer.overlappedStarts=0` con `commands.peakOutstanding=2` | La evidencia de P3 los interpretó como ejecución simultánea; se ha corregido. La ejecución simultánea se mide con `sequencer.overlappedStarts` y `sequencer.peakOutstanding`. |
| 2 | Las estadísticas por ciclo de Minor (`vectorWait*Cycles`, `vectorActiveCommandsDistribution`, `vectorMultiActiveCycles`) sólo se muestrean cuando la pipeline se evalúa; si Minor está inactiva (`pipeline.cc`, `activityRecorder`) esos ciclos se pierden. | La distribución suma 30 442 ciclos de 52 083 | `vectorWaitCompletionCycles` (25 968) infravalora la espera real. |
| 3 | `sequencer.rawWaitTicks`, `warWaitTicks` y `wawWaitTicks` suman el mismo intervalo en cada tipo presente. | Los tres valen 1 860 000; el código suma `elapsed` a cada uno | No son exclusivos: su suma triplica la espera. La descripción no lo dice. |
| 4 | `commands.*`, `queue.*` y `vectorInterface.*` dan los mismos valores en Pathfinder. | Comparación directa | Miden puntos distintos del mismo recorrido, pero sin carga que los separe parecen duplicados; documentar qué intervalo cubre cada uno. |
| 5 | `lrf*.waitCycles`, `capacityWaitCycles` y `portWaitCycles` se solapan (sus descripciones lo indican). | Los tres valen 372 por banco | Correcto, pero no deben sumarse. |
| 6 | `vectorBlocked*` (Minor) cuentan evaluaciones, no ciclos ni instrucciones, y una misma espera se cuenta en cada ciclo evaluado. | 24 833 bloqueos por capacidad en 52 083 ciclos | Útiles como proporción; su unidad debería decirlo. |
| 7 | Las métricas vectoriales de Minor usan `prereq` y no aparecen cuando valen 0. | `vectorWaitGrantCycles` ausente | Un cero no se distingue de una métrica inexistente; `run.py` lo trata como 0 en offload. |

## Correcciones propuestas

Propuestas en P4 y aplicadas después con aprobación (ver la sección
siguiente):

1. **Minor:** describir `vectorMultiActiveCycles` y
   `vectorActiveCommandsDistribution` como comandos "en la VPU" (despachados
   y sin terminar), no "activos".
2. **Minor:** contar las esperas vectoriales por intervalos (como hace el
   secuenciador con `headWait`) en lugar de por ciclo evaluado, o
   actualizarlas al despertar la pipeline, para no perder ciclos inactivos.
3. **Secuenciador:** indicar en las descripciones de `raw/war/wawWaitTicks`
   que se solapan y añadir `hazardWaitTicks` con el intervalo total.
4. **Documentación de estadísticas:** una tabla que diga qué intervalo mide
   cada grupo (`vectorInterface`, `queue`, `commands`, `sequencer`) y qué
   métricas se solapan.
5. **Minor:** quitar `prereq` de las métricas vectoriales o documentarlo.

Para el criterio 1 y para observar ALUs y puertos hace falta una carga con
instrucciones aritméticas independientes; Pathfinder no la tiene.


## Correcciones aplicadas

Aprobadas y aplicadas el 11 de noviembre de 2026 (commit
«cpu: Count Minor vector waits by intervals»):

| # | Cambio | Comprobación con Pathfinder |
| --- | --- | --- |
| 1 | Descripciones de Minor: `vectorMultiActiveCycles` y la distribución hablan de comandos "en la VPU", incluidos los de la cola; `vectorBlocked*` cuentan evaluaciones de Execute. | — |
| 2 | Minor cuenta sus esperas vectoriales por intervalos (`Execute::sampleVectorWaits`): cada evaluación atribuye los ciclos transcurridos al estado anterior, incluidos los ciclos en que la pipeline duerme. | La distribución suma `numCycles` exactamente (`tiny`: 52 083; `small`: 5 995 385). `vectorWaitCompletionCycles` pasa de 25 968 a 37 583 en `tiny`, coherente con `commands.busyTicks` (38 327 ciclos). |
| 3 | Secuenciador: `hazardWaitTicks` cuenta una vez cada espera por dependencias; `raw/war/wawWaitTicks` avisan de que se solapan. | `hazardWaitTicks` = 1 860 000 en `tiny`, no el triple. |
| 4 | Tabla de grupos, en la sección siguiente. | — |
| 5 | Con offload, las métricas vectoriales de Minor se escriben aunque valgan 0; sin offload siguen ocultas mientras valgan 0, para no añadir líneas a otras simulaciones de Minor. | `vectorWaitGrantCycles 0` aparece en offload; en native no hay líneas vectoriales. |

La temporización no cambia: los ticks de `tiny` y `small` en ambos modos
son idénticos a los de P3. El matiz 4 de la [revisión de extremo a
extremo](<Revisión de extremo a extremo - Minor y VPU.md>) (intervalos
abiertos del secuenciador al volcar) sigue pendiente.

## Qué intervalo mide cada grupo

Los cuatro grupos siguen el mismo comando en puntos distintos. En
Pathfinder dan los mismos valores porque la reserva y el despacho ocurren
en el mismo ciclo y la finalización llega a la CPU en el mismo tick; en
otras cargas pueden diferir.

| Grupo | Abre | Cierra | Incluye |
| --- | --- | --- | --- |
| `vectorInterface` | `CpuVectorInterface::dispatch` (la CPU entrega el comando) | Entrega de la finalización a la CPU | Cola, ejecución y aviso a la CPU |
| `queue` | Reserva concedida (`CommandQueue::requestGrant`) | `CommandQueue::release`, al terminar | Tiempo entre reserva y despacho, cola y ejecución |
| `commands` | `VectorEngine::dispatch` | `VectorEngine::completed` | Cola y ejecución |
| `sequencer` | Inicio del comando (`AraSequencer::start`) | Finalización | Sólo ejecución; no incluye la espera en cola |

Reglas para leerlos:

- La ejecución simultánea se mide con `sequencer.peakOutstanding`,
  `sequencer.overlappedStarts` y `sequencer.multiActiveTicks`. Los
  máximos de `commands`, `queue` y `vectorInterface`, y las métricas
  "en la VPU" de Minor, incluyen comandos en cola.
- `sequencer.capacityWaitTicks`, `memoryBarrierWaitTicks`,
  `isolationWaitTicks` y `hazardWaitTicks` pueden solaparse entre sí;
  `raw/war/wawWaitTicks` se solapan con `hazardWaitTicks` y entre sí.
- `lrf*.waitCycles`, `capacityWaitCycles` y `portWaitCycles` se solapan.
- Los `busyTicks` de recursos distintos se solapan y no suman el tiempo
  total; `workTicks` suma la ocupación de cada elemento en curso.

[plan-f8]: <Plan de cierre de la fase 8.md>
[benchmark]: <../../benchmarks/pathfinder/README.md>
[cierre]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
