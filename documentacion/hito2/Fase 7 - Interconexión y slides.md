# Fase 7 del hito 2: interconexión entre lanes y slides, entrega VPU

Implementación autorizada por el usuario el 5 de noviembre de 2026, sobre
`ced76afb32`, con el alcance de la propuesta presentada en la conversación
y sus decisiones:

- no se modifica Minor;
- las slides viajan en un payload propio, `SlideCommand`;
- las ejecuta una unidad global, `SlideUnit`, en `vpu/slide/`;
- hay una sola tarea de slide activa a la vez;
- `interconnect_enabled` se sustituye por `interconnect_model`.

La entrega atiende O2 y O3:

- **O2:** buffers de red y de slide finitos, contrapresión con `Retry` y
  servicio y tránsito separados.
- **O3:** primer camino de datos entre lanes detrás de una interfaz
  independiente de la topología, con métricas por enlace comparables entre
  organizaciones. Es la base para estudiar agrupaciones de lanes en el
  hito 3.

## Alcance y estado de la evidencia

La VPU sabe planificar, temporizar y cerrar:

- `vslide1up.vx` y `vslide1down.vx` sin predicación, con SEW 8/16/32/64 y
  todos los LMUL del perfil. Se cubren `vstart`, rangos vacíos, `vl=1`,
  tail, inserción del escalar y el solapamiento legal `vd == vs2` de down.
- Un crossbar lógico detrás de `VectorInterconnect`, con capacidad por
  destino, límite de inyección por origen, round-robin por destino,
  servicio en bytes/ciclo y latencia de tránsito.
- Métricas comunes de transporte y tráfico por enlace.

**Frontera con Minor.** No se modifica la CPU. Minor sigue decodificando
sólo `vadd.vv/.vx`, `vle32.v` y `vse32.v`. Por tanto, **ningún programa
alcanza todavía las slides** y la red no transporta tráfico en las cargas
actuales. Los pendientes de CPU están en la
[sección 2.7 de los pendientes de Minor][pendientes-minor]. El criterio 6
de la [sección 15.3][cierre] (slides desde programa, con la red en los
casos que cruzan lanes) queda pendiente de esa parte. El criterio 14
(interfaz independiente de topología, crossbar detrás y métricas comunes)
está implementado, pero sin evidencia de ejecución.

**Estado posterior (11 de noviembre de 2026).** La rama de Minor,
integrada en `feature/vpu` hasta `963e6a9550`, decodifica
`vslide1up.vx` y `vslide1down.vx` sin descomponerlas, construye
`SlideCommand` con `vd`, `vs2` y el escalar `rs1` capturado una vez, y
convierte en `IllegalInstFault` los rechazos de admisión (`vd == vs2` en
up, `vm=0`) (`afa2191d8c`). Verificado en código, sin ejecución
registrada: el criterio 6 sigue pendiente de evidencia desde programa y la
red aún no ha transportado tráfico de una carga.

Compilar esta entrega no acredita ejecución funcional ni validación
temporal. No se han ejecutado cargas ni se han creado pruebas.

## Archivos

Bajo `src/cpu/vector_engine/`:

| Archivos | Responsabilidad |
| --- | --- |
| `common/vector_types.hh` | `SlideOperation` y `VectorUnitClass::Slide`. |
| `interface/vector_command.hh` | `SlideCommand` y nueva alternativa de `VectorCommandPayload`. |
| `common/operation_traits.hh/.cc` | `ExecutionClass::Slide`, `destinationMayOverlapSource`, fichas de las slides y sus operandos. |
| `common/vector_support.hh/.cc` | `supportsSlide`, grupos y solapamiento en `checkStructure`, admisión en `checkCommand`. |
| `common/backend_task.hh` | `SlideTask`. |
| `common/vrf_types.hh` | `VrfRequesterKind::Slide`. |
| `common/interconnect_types.hh` | `TransferKeyHash`, `InterconnectModelKind`. |
| `common/vpu_parameters.hh` | Modelo de red en `InterconnectParameters`; `SlideParameters`. |
| `frontend/ara_sequencer.hh/.cc` | Creación, envío y recuento de `SlideTask`. |
| `vpu/register_file/lane_register_file.hh/.cc` | Tercer puerto de respuesta, para `SlideUnit`. |
| `vpu/interconnect/vector_interconnect.hh/.cc` | Contrato de reintento y fábrica `makeInterconnect`. |
| `vpu/interconnect/crossbar_interconnect.hh/.cc` | Nuevo: modelo crossbar. |
| `vpu/interconnect/interconnect_stats.hh/.cc` | Nuevo: métricas comunes a cualquier topología. |
| `vpu/slide/slide_unit.hh/.cc` | Nuevo: ejecutor de las dos slides. |
| `vector_engine.hh/.cc` | Construcción de la red y de `SlideUnit`, enlaces, validación y drain. |
| `VectorEngine.py`, `configuration.py`, `SConscript` | Parámetros, configuración declarativa y fuentes. |

Fuera de ese directorio sólo cambia `configs/example/riscv_vpu.py`. No se
modifican `src/cpu/minor/`, `src/arch/`, `AraLane`, ALU, FPU, VLSU,
`future/`, `build/` ni `ext/`.

## Semántica implementada

Referencia: RVV 1.0, edición 20240411, secciones *Vector Slide
Instructions*, *Vector Slide1up* y *Vector Slide1down Instruction*.

Sea `s` el escalar que la CPU captura y normaliza a SEW. Para cada índice
activo de destino `i` en `[vstart, vl)`:

| Operación | Valor escrito |
| --- | --- |
| `vslide1up.vx` | `s` en `i=0` (sólo si `vstart=0`); `vs2[i-1]` en los demás. |
| `vslide1down.vx` | `s` en `i=vl-1`; `vs2[i+1]` en los demás. |

- **Fuentes.** Se derivan del desplazamiento, no del rango de destino. Up
  lee `vs2[max(vstart,1)-1 .. vl-2]` y, si `vstart>0`, incluye
  `vs2[vstart-1]`. Down lee `vs2[vstart+1 .. vl-1]`. Como `vl <= VLMAX`,
  ninguna lectura sale del grupo.
- **Rango vacío.** Con `vl=0` o `vstart>=vl`, el sequencer completa sin
  crear tarea, como en el resto de operaciones.
- **Prestart y tail.** Las escrituras sólo habilitan los bytes de
  `[vstart, vl)`. Se conservan los elementos anteriores a `vstart` y el
  tail; conservarlo también es válido con `vta=1`.
- **Escalar.** Se escribe con el orden de bytes del objetivo y sólo se usan
  sus SEW bits bajos. La extensión de signo con XLEN < SEW corresponde a la
  CPU, mediante `ElementCodec::normalizeScalar`.
- **Solapamiento.** Admisión devuelve `InvalidRegisterGroup` para up con
  `vd == vs2`, porque RVV reserva esa codificación. Down lo admite (ver
  [Solapamiento legal en down](#solapamiento-legal-en-down)). Con el mismo
  LMUL y grupos alineados, dos grupos coinciden o son disjuntos.
- **Máscara.** Las formas `vm=0` se rechazan antes de producir efectos
  (`UnsupportedConfiguration`, como toda forma predicada). No se ejecutan
  como si todos los elementos estuvieran activos.
- No hay cálculo FP ni interpretación de signo: se mueven patrones de bits.
  Una slide no lleva `fpContext`.

## Organización

### SlideUnit

Es una unidad global, como la SLDU de Ara, con un agente por lane. Cada
agente ofrece accesos a su LRF por `slide_vrf_inputs_per_lane` entradas
persistentes y recibe de la red los paquetes dirigidos a su lane. Hay una
sola tarea activa; otra recibe `Retry` del sequencer.

**Plan.** Al aceptar la tarea, el mapper divide el destino en palabras.
Cada palabra destino se forma con **piezas**: bytes de una palabra fuente
que van a esa palabra destino. Una pieza es:

- **local** si fuente y destino están en la misma lane: se copia al llegar
  la lectura, sin red;
- **remota** si están en lanes distintas: viaja en paquetes de como máximo
  `maxPacketBytes`, con `TransferKey = (TaskKey, índice de pieza)`.

Con el mapeo `word_striped`, cada palabra destino suele tener una parte
local (su propia palabra fuente, desplazada) y otra remota de un elemento
(la palabra fuente vecina). Con SEW igual a la palabra, toda la palabra
llega de la lane vecina. Con una sola lane todo es local y la red no se
usa.

**Recursos por lane.**

| Recurso | Se reserva | Se libera |
| --- | --- | --- |
| Buffer de ensamblado (`slide_assembly_buffers_per_lane`, ≥ 2) | Para una palabra destino, en orden ascendente, antes de leer sus fuentes. Incluye la recepción de sus piezas remotas. | Con el `WriteAck` de la palabra. |
| Buffer de fuente (`slide_source_buffers_per_lane`) | Al publicar la lectura de una palabra fuente, cuando todos sus destinos tienen ensamblado. | Cuando sus piezas locales se han copiado y las remotas se han aceptado en la red, que guarda su copia. |

Las lecturas se publican en orden ascendente. Cada fuente alimenta como
mucho dos destinos consecutivos y hay al menos dos buffers de ensamblado
por lane, así que la palabra destino más antigua siempre progresa. Como la
recepción está reservada con el ensamblado, `reserveReception()` siempre
acepta y la red siempre puede entregar.

**Identidades.**

- `VrfAccessKey` lleva `requester = Slide`, con `accessId` 2k para leer la
  fuente k y 2j+1 para escribir el destino j.
- Los paquetes llevan `TransferKey`.
- Las respuestas y entregas se localizan por clave, nunca por orden de
  llegada.

**Cierre.** La tarea termina con el último `WriteAck`. Recibir un paquete
no completa la escritura de su palabra.

**Ruta local.** Una pieza local no consume red ni ancho de banda de
transporte: sus bytes pasan del buffer de fuente al de ensamblado de la
misma lane al llegar la lectura, sin ciclos adicionales. Sí consume la
lectura y la escritura del VRF y ambos buffers. Las métricas
`slide.localBytes` y `slide.remoteBytes` separan los dos caminos para
interpretar los resultados.

### Solapamiento legal en down

Con `vd == vs2`, la escritura de una palabra destino espera a que se hayan
leído todas las palabras fuente que solapan sus bytes. Se cuenta en
`slide.aliasWriteStalls`.

Con SEW menor que la palabra, la regla ya se cumple por construcción,
porque la palabra destino necesita sus propios bytes. Con SEW igual a la
palabra, la escritura `d` toma la palabra `d+1` entera y debe esperar a
que se lea la `d`, que necesita el destino `d-1`.

La regla no depende del orden de entregas de la red. El scoreboard no
interviene: no ve riesgos internos de un comando.

### Crossbar

Es la primera implementación de `VectorInterconnect` (sección 10.1 del
[diseño][diseno]). No es una NoC ni reproduce el cableado de Ara.

| Aspecto | Comportamiento |
| --- | --- |
| Inyección | Cada origen acepta un paquete cada `ceil(bytes / source_bytes_per_cycle)` ciclos. |
| Capacidad por destino | `packets_per_destination` paquetes y `bytes_per_destination` bytes aceptados y no entregados. El receptor reserva su propio espacio. |
| Servicio | Un paquete cada vez por destino, elegido por round-robin entre orígenes con espera; ocupa `ceil(bytes / destination_bytes_per_cycle)` ciclos. Destinos distintos se atienden en paralelo. |
| Tránsito | La entrega ocurre `transit_latency` ciclos después del servicio, con el paquete entero. |
| Rechazo | `Retry` sin efectos si el origen está inyectando, el destino está lleno o el receptor no reserva. |

**Reintento.** El emisor conserva paquete y clave y vuelve a llamar a
`send()` desde su propio evento, en el ciclo siguiente. La red no avisa de
capacidad libre, de modo que el emisor no depende de detalles internos.
Tras un `Retry`, `SlideUnit` no intenta más paquetes de esa lane origen en
el mismo ciclo; otras lanes siguen enviando.

**Selección.** El modelo se elige al construir `VectorEngine` mediante
`makeInterconnect()`. `SlideUnit` sólo ve `VectorInterconnect` e
`InterconnectReceiver`. La red se construye siempre y la VLSU no la usa:
la memoria de fase 4 sigue accediendo a cada LRF directamente.

**Drain.** `VectorEngine::isIdle()` incluye red y `SlideUnit`. Drain no
rechaza paquetes de tareas ya admitidas.

### Sequencer

`SlideTask` cubre el rango activo de destino y se envía a `SlideUnit`. Una
slide no es barrera de memoria: respeta capacidad, aislamiento con
`vstart ≠ 0` y el scoreboard, que reserva `vd` como escritura y `vs2` como
lectura. Puede solaparse con aritmética independiente.

### Diferencias con Ara

- La SLDU de Ara implementa todas las slides y otras permutaciones; aquí
  sólo hay desplazamientos de un elemento.
- En Ara los operandos van de las lanes a la SLDU y vuelven por su propio
  cableado. Aquí sólo cruzan la red las piezas cuyo origen y destino están
  en lanes distintas.
- El crossbar no modela el cableado físico de Ara ni su latencia.

## Parámetros

| Parámetro | Por defecto | Significado |
| --- | --- | --- |
| `interconnect_model` | `"crossbar"` | Modelo de red; sustituye a `interconnect_enabled`, eliminado. |
| `interconnect_max_packet_bytes` | 0 (= palabra) | Máximo de bytes por paquete; las piezas mayores se dividen. |
| `interconnect_packets_per_destination` | 1 | Paquetes aceptados y no entregados por destino. |
| `interconnect_bytes_per_destination` | 0 (= palabra) | Bytes aceptados y no entregados por destino. |
| `interconnect_source_bytes_per_cycle` | 0 (= palabra) | Inyección por origen. |
| `interconnect_destination_bytes_per_cycle` | 0 (= palabra) | Servicio por destino. |
| `interconnect_transit_latency` | 1 | Ciclos tras el servicio. |
| `slide_vrf_inputs_per_lane` | 1 | Entradas de `SlideUnit` en cada LRF. |
| `slide_source_buffers_per_lane` | 1 | Palabras fuente leídas por lane. |
| `slide_assembly_buffers_per_lane` | 2 | Palabras destino en construcción por lane; mínimo dos. |

En `configuration.py`, `InterconnectConfiguration.enabled` pasa a `model`
y se añade `SlideConfiguration` (`VpuConfiguration.slide`). El ejemplo
`riscv_vpu.py` añade:

- `--vpu-interconnect-transit-latency`
- `--vpu-interconnect-packets-per-destination`
- `--vpu-interconnect-bytes-per-cycle` (inyección y servicio)
- `--vpu-slide-source-buffers`
- `--vpu-slide-assembly-buffers`

**Cambio incompatible.** Las configuraciones que pasen
`interconnect_enabled` o `InterconnectConfiguration(enabled=...)` deben
eliminarlo. Se retira también la restricción que obligaba a que la red
estuviera deshabilitada.

## Estadísticas

Se elimina `interconnect.enabled`. La referencia del hito 1 en
[Estadísticas.md](<../hito1/Estadísticas.md>) describe el estado anterior.

| Grupo | Métricas |
| --- | --- |
| `interconnect` (común a cualquier topología) | `accepted`, `completed` (entregados), `bytes`, `retries`, `outstanding`, `peakOutstanding`, `busyTicks`, `workTicks`; `attempts`, `admissionWaitTicks` (primer intento → aceptación), `transportTicks` (aceptación → entrega), `deliveredBytes`, `localPackets`/`remotePackets`, `localBytes`/`remoteBytes`, `linkPackets` y `linkBytes` (matrices origen × destino). |
| `interconnect.crossbar` (propias del modelo) | `sourceBusyRetries`, `destinationFullRetries`, `receiverRetries`, `contentionWaitTicks` (aceptación → servicio), `serviceTicks`, `peakDestinationPackets`. |
| `slide` | Contadores de tarea de `ModuleStats`; `slide1UpTasks`, `slide1DownTasks`, `sourceReads`, `destinationWrites`, `localBytes`, `remoteBytes`, `packetsAccepted`, `packetRetries`, `scalarInsertions`, `assemblyStalls`, `sourceBufferStalls`, `aliasWriteStalls`. |
| `sequencer` | `slideCommands`. |

Los contadores `*Stalls` cuentan evaluaciones de la unidad, no ciclos. Las
métricas comunes sólo observan los eventos `Attempt`, `Accepted` y
`Delivered`. Otra topología puede añadir las suyas sin cambiar éstas.

## Referencia normativa

RVV 1.0, manual oficial edición 20240411, [capítulo vectorial][rvv].
Secciones consultadas:

- *Vector Slide Instructions*: rango activo, `vstart` y prohibición de
  solapamiento en slideup.
- *Vector Slide1up*: escalar en el índice 0 si está activo; extensión o
  truncado del escalar según XLEN.
- *Vector Slide1down Instruction*: escalar en `vl-1`.

Topología, buffers, latencias y orden de lectura son decisiones del modelo.

## Comprobaciones y pendientes

Comprobaciones realizadas:

- `git diff --check`: correcto.
- `clang-format --dry-run --Werror` (20.1.8) sobre los C++ modificados y
  nuevos: correcto.
- Black 25.11.0 e isort 7.0.0, en un entorno temporal, sobre los tres
  Python: correctos. `python3 -m py_compile`: correcto.
- SCons: compilación y enlace de `build/RISCV/gem5.opt`, que recompila
  también Minor con las cabeceras modificadas. SCons avisa de que GCC
  15.2.1 está fuera de su rango; los avisos `-Warray-bounds` proceden de
  `base/statistics.hh` en `vector_reg_bank.cc` y `lane_register_file.cc`,
  como en la fase 6.
- `configs/example/riscv_vpu.py --help` muestra las opciones nuevas.
- `VectorEngine.from_configuration` crea la configuración con valores no
  por defecto (4 lanes, paquetes de 4 bytes, latencia 3, buffers de slide
  2 y 3) y rechaza el parámetro eliminado `interconnect_enabled`. No se
  llamó a `m5.instantiate`, así que la validación del constructor C++ no
  se ha ejecutado. No se ha ejecutado ninguna carga.

Para el equipo se sugieren, sin implementar, y con comandos inyectados en
la VPU hasta que exista la parte de Minor:

- **Slides:** las dos operaciones en los cuatro anchos con 1, 2 y 4
  lanes; `vl=0`, `vl=1`, `vstart>0` en up (lectura de `vs2[vstart-1]`) y
  en down; tail conservado; escalar más ancho y más estrecho que SEW;
  rechazo de up con `vd == vs2`; down con `vd == vs2` y SEW igual a la
  palabra.
- **Crossbar:** varias fuentes hacia un destino, destinos independientes
  en paralelo y receptor o destino lleno. Comprobar que no hay pérdidas ni
  duplicados (`interconnect.accepted == interconnect.completed` y matriz
  `linkBytes` coherente).

Quedan pendientes:

- la parte CPU de 2.7 ([pendientes de Minor][pendientes-minor]) y, con
  ella, la evidencia desde un programa (actualización del 11 de noviembre
  de 2026: la parte CPU está implementada; falta la evidencia);
- varias slides activas;
- envío de una pieza antes de que su lectura completa termine (no hay
  chaining).

Quedan fuera: predicación (`vm=0`), slides de distancia variable, formas
`.vf`, gathers, SLDU completa, otras topologías, routers y flits.

[diseno]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md>
[cierre]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[pendientes-minor]: <Pendientes de Minor - Fases 1 a 5.md#fase-27-slides>
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
