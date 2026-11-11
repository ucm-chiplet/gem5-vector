# Fase 5 del hito 2: concurrencia en la VPU, entrega VPU

Implementación autorizada por el usuario el 4 de noviembre de 2026, sobre
`46f1669d13`, con el alcance de la propuesta presentada en la conversación
y sus ajustes: sólo se modifica la VPU, la concurrencia queda activada por
defecto y el ejemplo `configs/example/riscv_vpu.py` expone sus parámetros.
La entrega atiende O2 (contextos, créditos y dependencias finitos y
observables) y O6 (métricas de solapamiento y de causas de espera).

## Alcance y estado de la evidencia

La VPU puede mantener varios comandos aritméticos independientes en
ejecución, desde `CommandQueue` hasta las lanes:

- `CommandQueue` separa comandos pendientes e iniciados y conserva el
  crédito de cada uno hasta entregar su finalización.
- Un scoreboard arquitectónico conservador bloquea RAW, WAR y WAW por
  grupos de registros completos.
- `AraSequencer` inicia en orden hasta `max_active_commands` comandos y
  entrega una finalización por comando, en el orden en que terminen.
- `TaskDistributor` y `AraLane` mantienen varias tareas con estado propio.

**Frontera con Minor.** No se modifica la CPU. Minor sigue despachando un
comando VPU sólo cuando su instrucción es la cabeza de retirada y no
despacha el siguiente hasta retirarla. Por tanto, **un programa no produce
todavía comandos simultáneos en la VPU** y los resultados y tiempos de los
programas actuales no cambian por esta entrega. El criterio de cierre de la
fase conjunta (Minor despacha más de una instrucción, sección 13.5 del
[diseño][diseno]) y las acciones CPU de la fase 2.5 de la [guía de
responsabilidades][responsabilidades] quedan pendientes.

**Estado posterior (11 de noviembre de 2026).** La rama de Minor,
integrada en `feature/vpu` por fast-forward hasta `963e6a9550`, añade la
parte CPU de 2.5 según su [diseño de concurrencia][concurrencia-minor]
(`3686e7e151`, `4c48d2de3b`, `91dac1586f`, `88bec9237b`). Verificado en
código, sin ejecución registrada:

- `advanceVectorDispatch()` despacha fuera de la cabeza de retirada,
  dentro de una ventana de `maxActiveVectorOffloads` entradas (2 por
  defecto) y hasta `vectorDispatchWidth` comandos por ciclo (1 por
  defecto).
- `accepted()` y `completed()` localizan la entrada por `CommandKey`.
- Descarte y drain inspeccionan todas las entradas de `vpuOffloadStates`.
- Estadísticas de espera y ocupación en `MinorStats`.

Las dos ventanas son distintas: Minor admite 2 instrucciones en vuelo y la
VPU 4 comandos activos por defecto. Su coherencia de extremo a extremo se
revisa en el paquete P5 del [plan de cierre de la fase 8](
<Plan de cierre de la fase 8.md>). El estado detallado de la parte CPU
está en los [pendientes de Minor][pendientes-minor].

No hay chaining, renombramiento, ROB, solapamiento con memoria, varias
instrucciones de memoria ni predicación. Compilar esta entrega no acredita
ejecución funcional ni validación temporal del solapamiento.

## Archivos

Bajo `src/cpu/vector_engine/`:

| Archivos | Responsabilidad |
| --- | --- |
| `frontend/command_queue.hh/.cc` | Pendientes, iniciados, `markStarted` y `release(key)`. |
| Nuevo `frontend/vector_scoreboard.hh/.cc` | Reservas de lectura/escritura por grupos y riesgos. |
| `frontend/SConscript` | Registro del archivo nuevo. |
| `frontend/ara_sequencer.hh/.cc` | Varios activos, reglas de inicio, cierres desordenados y métricas. |
| `frontend/task_distributor.hh/.cc` | Varias tareas por `TaskKey` y envío por antigüedad en cada lane. |
| `vpu/lanes/ara_lane.hh/.cc` | Fragmentos de varias tareas, orden por admisión y `accessId` derivado. |
| `vector_engine.cc` | Validación de capacidad y conexión. |
| `VectorEngine.py`, `configuration.py` | Valores por defecto y descripción. |

Fuera de la VPU sólo cambia `configs/example/riscv_vpu.py`. No se modifican
Minor, `src/arch/`, `interface/`, VLSU, backend de memoria, VRF, ALU, FPU,
`future/` ni `build/`.

## Capacidad y estados

Cada comando está en un único estado de la cola:

```text
reservas + pendientes + iniciados <= command_queue_entries
iniciados <= max_active_commands
```

Iniciar un comando lo saca de pendientes sin devolver su crédito. Al
cerrarlo, el sequencer libera su reserva del scoreboard, notifica
`completed` a la CPU y después llama a `release(key)`. Así la CPU todavía
ve el comando pendiente durante el callback. `CommandQueue::isIdle()`
incluye los iniciados.

`VectorEngine` exige `1 <= max_active_commands <= command_queue_entries`.
El distribuidor acepta hasta `max_active_commands` tareas: cada comando
aritmético tiene como máximo una, de modo que no se añade otro parámetro.

## Reglas de inicio

Sólo la cabeza de pendientes puede iniciar; no hay bypass. Se inicia como
máximo un comando por ciclo, y un cierre libera recursos para iniciar a
partir del ciclo siguiente, como en la entrega serie. La cabeza espera si:

| Causa | Condición |
| --- | --- |
| Capacidad | Hay `max_active_commands` comandos activos. |
| Barrera de memoria | La cabeza es carga/store y hay activos, o hay una carga/store activa. |
| Aislamiento | La cabeza tiene `vstart != 0` y hay activos, o hay un activo con `vstart != 0`. |
| RAW | La cabeza lee un registro que escribe un activo. |
| WAR | La cabeza escribe un registro que lee un activo. |
| WAW | La cabeza escribe un registro que escribe un activo. |

La VPU aplica por sí misma la barrera de memoria (sección 7.1 del
[diseño][diseno]) y el aislamiento de `vstart` no nulo (sección 7), sin
depender de que Minor los respete. Un rango vacío (`vstart >= vl`) sigue
las mismas reglas y se cierra sin tarea en la evaluación siguiente.

## Scoreboard

`VectorScoreboard::footprint()` obtiene los registros a partir de
`describeOperands()`: cada operando vectorial marca su grupo completo
(`firstReg` a `firstReg + regCount - 1`) como leído, escrito o ambos. En
LMUL fraccional `regCount` es 1 y se reserva el registro contenedor. Los
escalares capturados y las direcciones no ocupan registros.

La reserva se toma al iniciar y sólo se compara con comandos activos, que
son anteriores porque el inicio es en orden. Un comando no se compara
consigo mismo. Las lecturas compartidas no generan conflicto y los grupos
que coinciden en parte sí. La reserva se libera cuando la unidad entrega su
finalización, es decir, tras confirmar todos los `WriteAck`: el consumidor
nunca empieza con resultados parciales.

Liberar en la VPU sin esperar a la retirada de Minor es seguro porque un
comando aritmético aceptado no puede fallar ni anularse en el hito 2.

## Envío, distribución y lanes

El sequencer envía las tareas en orden de inicio. Si una unidad responde
`Retry`, conserva el descriptor y la `TaskKey` y no envía las posteriores
hasta que la unidad acepte ésa. Las finalizaciones se localizan por
`CommandKey` y se validan contra su `TaskKey`.

En cada evaluación, el distribuidor intenta un envío por lane: el siguiente
fragmento de la tarea más antigua con fragmentos para esa lane. Una tarea
joven no adelanta a otra anterior en la misma lane, aunque ésta reciba
`Retry`. Las tareas terminadas se cierran en cualquier orden.

La lane admite fragmentos de varias tareas en sus `lane_task_entries`
contextos. Los ordena por admisión, que coincide con la antigüedad porque
el distribuidor entrega en orden. Se conserva el inicio de un bundle por
ciclo y la regla de fase 3: un fragmento joven listo no adelanta a otro
anterior que aún reúne operandos.

El `accessId` de cada acceso al VRF pasa a ser `fragmentId * 3 + papel`
(LHS, RHS, destino). Es único dentro de su `TaskKey` y lane, no depende de
un contador reiniciado al cambiar de tarea y sigue siendo independiente
del `requestId` de memoria.

No hay bloqueo mutuo entre tareas: las activas no dependen entre sí, de
modo que una tarea que ocupa contextos o buffers termina y los libera.

## Parámetros y valores por defecto

| Parámetro | Por defecto | Unidad y condición |
| --- | --- | --- |
| `command_queue_entries` | 4 (antes 1) | Reservas + pendientes + iniciados; positivo. |
| `max_active_commands` | 4 (antes 1) | Comandos iniciados sin finalizar; `<= command_queue_entries`. |
| `lane_task_entries` | 4 (antes 1) | Contextos por lane, compartidos entre tareas; positivo. |

Con cuatro contextos por lane cada comando activo puede ocupar uno en cada
lane. El valor anticipa la fase 6, donde una operación FPU y otra ALU
independientes deben progresar en la misma lane. Cambiar
`lane_task_entries` también permite más fragmentos en vuelo de una sola
instrucción, por lo que los ciclos de una instrucción aislada pueden
diferir de los de las fases 3--4 con sus valores por defecto.

`configs/example/riscv_vpu.py` añade `--vpu-command-queue-entries`,
`--vpu-max-active-commands` y `--vpu-lane-task-entries`, con los valores
por defecto de `VpuConfiguration` y `LaneConfiguration`.

## Estadísticas

Grupo `sequencer`:

| Métrica | Qué mide |
| --- | --- |
| `accepted`, `completed`, `outstanding`, `peakOutstanding` | Comandos iniciados, cerrados, activos y máximo de activos. |
| `overlappedStarts` | Comandos iniciados mientras había otro activo. |
| `multiActiveTicks` | Ticks con dos o más activos; se acumula en cada cambio de ocupación. |
| `capacityWaitTicks` | Ticks de espera de la cabeza por capacidad. |
| `memoryBarrierWaitTicks` | Ticks de espera de la cabeza por barrera de memoria. |
| `isolationWaitTicks` | Ticks de espera por aislamiento de `vstart` no nulo. |
| `rawWaitTicks`, `warWaitTicks`, `wawWaitTicks` | Ticks de espera por cada riesgo. |

Las causas de espera no son excluyentes y no deben sumarse como tiempo
total. Se reparten en cada evaluación de la cabeza, que ocurre al recibir
un dispatch o una finalización; la cabeza bloqueada no se sondea en cada
ciclo. Un intervalo abierto al volcar estadísticas se contabiliza en el
cambio siguiente.

El distribuidor conserva `accepted`, `outstanding` y `peakOutstanding` por
tarea, y la lane sus métricas de fase 3, ahora con fragmentos de varias
tareas.

## Referencia normativa

RVV 1.0, manual oficial edición 20240411, [capítulo vectorial][rvv].
Secciones consultadas: *Vector Register Grouping* (grupos y alineamiento
para reservar registros) y *Vector Start Index CSR* (`vstart`). La entrega
no añade ni cambia instrucciones: el solapamiento, la reserva conservadora
y el aislamiento son decisiones del modelo, no requisitos de RVV.

## Comprobaciones y pendientes

Comprobaciones realizadas:

- `git diff --check`: correcto.
- `clang-format --dry-run --Werror` sobre los C++ modificados y nuevos:
  correcto.
- Black 25.11.0 e isort 7.0.0 (versiones de `.pre-commit-config.yaml`),
  en un entorno temporal, sobre los tres Python: correctos.
  `python3 -m py_compile`: correcto.
- SCons con objetivos explícitos bajo `build/RISCV/`: cola, scoreboard,
  sequencer, distribuidor, lane, propietario y Python embebido de
  `VectorEngine` y `configuration`: compilación correcta, sin avisos del
  compilador. SCons avisa de que GCC 15.2.1 está fuera de su rango.
- Enlace de `build/RISCV/gem5.opt`: correcto. Con él,
  `configs/example/riscv_vpu.py --help` carga la configuración y muestra
  las tres opciones nuevas. No se ha ejecutado ninguna carga.

Para el equipo se sugieren, sin implementar, y con comandos inyectados en
la VPU hasta que exista la parte de Minor: dos sumas independientes con
intervalos activos solapados; cadena RAW; WAR/WAW con grupos `m2`/`m4`
parcialmente coincidentes; `command_queue_entries=2` saturada; `vl` largo
seguido de uno corto, para finalizaciones desordenadas; una carga entre
sumas como barrera; `vstart` no nulo aislado; rango vacío entre activos;
`lane_task_entries` 1 frente a 4; drain con varios comandos activos.

Quedan pendientes: acciones CPU de 2.5 (cursor de despacho, callbacks por
clave, retirada ordenada de varias entradas, drain e interrupciones en
Minor), y con ellas la evidencia de solapamiento desde un programa.
Actualización del 11 de noviembre de 2026: las acciones CPU están
implementadas (ver el estado posterior de la frontera con Minor); sigue
pendiente la evidencia de solapamiento desde un programa (criterio 1 de
la sección 15.3 del [diseño][diseno]).
Quedan fuera: chaining, memoria concurrente, FPU FP32, Mul/Div/resto y
varias ALUs de 2.6, red y slides de 2.7.

[diseno]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md>
[responsabilidades]: <Responsabilidades CPU y VPU.md>
[pendientes-minor]: <Pendientes de Minor - Fases 1 a 5.md>
[concurrencia-minor]: <Diseno de concurrencia MinorCPU - Fase 2.5.md>
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
