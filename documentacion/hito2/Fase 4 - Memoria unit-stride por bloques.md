# Fase 4 del hito 2: memoria unit-stride por bloques, entrega VPU

Implementación autorizada por el usuario el 3 de noviembre de 2026, sobre
`ec9e3c2f06e4`, con el alcance de la propuesta presentada en la
conversación y su ajuste: se elimina el modo por elemento y la memoria
unit-stride se sirve siempre por bloques. La entrega atiende O2 (buffers y
entradas VRF finitos), O1 (separar peticiones lógicas, paquetes, bytes y
accesos al VRF) y O6 (métricas trazables). No acredita reducción de tráfico
DRAM ni latencias calibradas de Ara.

## Alcance y estado de la evidencia

La VLSU recorre `[vstart, vl)` en bloques de elementos contiguos, con un
bloque activo y un paquete físico en vuelo:

- Formación de bloques recortados por capacidad, línea, página, final del
  rango y final del espacio de direcciones.
- Una petición lógica por bloque, con `(TaskKey, requestId)` propio.
- Reparto del bloque en accesos al VRF por palabra local, según el mapper.
- Reunión de stores por offset y reparto de cargas con espera de todos los
  `WriteAck` antes del bloque siguiente.
- Contrato de faults por bloque y métricas por bloque.

Se mantiene una instrucción activa. No hay bloques concurrentes,
solapamiento con aritmética, predicación ni nuevos modos de
direccionamiento. Los anchos y el perfil SEW/LMUL de fase 2 y los recursos
de fase 3 conservan su alcance. Compilar esta entrega no acredita ejecución
funcional ni validación temporal.

## Frontera CPU y archivos

No se modifican Minor, ISA, `interface/`, sequencer ni distribuidor. Minor
sigue recibiendo una finalización por comando, con `finalVstart` como
índice de elemento. Las acciones CPU de la fase 2.4 de la [guía de
responsabilidades](<Responsabilidades CPU y VPU.md>) (orden con LSQ, store
buffer y emisión anticipada) conservan su estado independiente; esta
entrega VPU no cierra por sí sola la fase conjunta.

Se modifican, bajo `src/cpu/vector_engine/`:

| Archivos | Responsabilidad |
| --- | --- |
| `common/memory_types.hh` | `MemoryBoundaries`, contrato de bloque y fault. |
| `common/vpu_parameters.hh` | Parámetros de bloque y entradas VLSU. |
| `VectorEngine.py`, `configuration.py` | Parámetros por módulo. |
| `vector_engine.cc` | Validación, consulta de fronteras y conexión. |
| `vpu/vlsu/ara_vlsu.hh/.cc` | Bloque activo, accesos por palabra y cierre. |
| `vpu/vlsu/vector_memory_backend.hh/.cc` | Bloques y fronteras. |
| `vpu/register_file/vrf_port.hh` | Comentario del productor VLSU. |
| Nuevo `vpu/vlsu/memory_block_planner.hh/.cc` | Formación de bloques. |
| `vpu/vlsu/SConscript` | Registro del archivo nuevo. |

## Formación de bloques

`planMemoryBlock()` es una función pura. Para el índice absoluto `i`, el
tamaño de elemento `E` y la base capturada calcula la dirección
`base + i * E`, reducida al ancho del objetivo, y:

```text
M = min(bytes restantes hasta vl, memory_max_block_bytes,
        bytes hasta fin de línea, bytes hasta fin de página,
        bytes hasta fin del espacio de direcciones)
elementCount = floor(M / E)
```

El rango del VRF empieza en `i * E`; `vstart` no desplaza la base. No se
accede a elementos fuera de `[vstart, vl)` y un rango vacío no crea
bloques. Los productos se calculan en 64 bits.

Si `elementCount` es cero, el elemento cruza una frontera y forma un bloque
de un elemento (`splitElement`). El backend lo divide en fragmentos
físicos y el bloque siguiente vuelve a agrupar. No es un modo alternativo:
es el resultado del mismo recorte.

Las fronteras de línea (`System::cacheLineSize`) y página (tabla de páginas
del proceso SE) las proporciona el backend mediante `boundaries()`. La VLSU
las consulta una vez al aceptar la tarea y no accede a `ThreadContext`.

Ejemplos con líneas de 64 bytes, máximo de 64 bytes y elementos de 4:

| Caso | Bloques |
| --- | --- |
| `vl=20`, base `0x1000` | 0--15 (64 B) y 16--19 (16 B) |
| Base `0x1030` | 16 B hasta la línea y después bloques de 64 B |
| Base `0x1FF0` | 0--3 en la página `0x1000`; 4 en adelante desde `0x2000` |

## Faults

RVV exige un trap preciso: se fija `vstart` al elemento que falla y los
anteriores quedan completados. El contrato de esta entrega es:

| Caso | Índice comunicado | Efectos |
| --- | --- | --- |
| Fallo de traducción en bloque de varios elementos | `firstElement` | El bloque está en una página: no se emite. |
| Error de acceso en bloque de varios elementos | `firstElement` | Un solo paquete en una línea: no se escribe ningún byte del bloque. |
| Cualquier fallo en bloque de un elemento | Ese elemento | Sus fragmentos se traducen antes del primer envío. |

`firstElement` pertenece siempre a la parte que falla, de modo que el
índice es válido sin inventar una localización fina que la respuesta no
proporciona. No se reenvía un store para localizar el fallo. El backend
comprueba con `panic` que un bloque de varios elementos produce un único
fragmento: si no fuera así, un fallo parcial no podría asociarse al índice.

Los bloques anteriores permanecen completados. Una carga fallida no
escribe en el VRF. Antes de comunicar `MemoryFault` no quedan paquetes,
traducciones ni accesos al VRF pendientes. Minor aplica `finalVstart` y
entrega el `Fault` a gem5; la VPU no escribe CSRs.

## Datos y VRF

Cada bloque reserva su buffer antes de cualquier acceso. El mapper reparte
`dataRange` en palabras locales; la VLSU comprueba que los rangos son
contiguos y cubren el bloque exactamente una vez. Cada palabra recibe un
`VrfAccessKey` propio (`accessId` consecutivo en el bloque), distinto del
`requestId` de memoria.

- **Store:** se publican todas las lecturas. Cada `ReadResponse` se copia
  en el offset de su acceso, independientemente del orden de llegada. Con
  el buffer completo se envía el bloque; ante `Retry` se conserva el mismo
  descriptor y buffer, sin releer el VRF. Se espera `StoreAck`.
- **Carga:** se envía el bloque y, con `LoadData`, se publican escrituras
  por palabra. El bloque termina con el último `WriteAck`.

Cada LRF tiene `memory_vrf_inputs_per_lane` entradas persistentes de la
VLSU. La entrada `k` ofrece el `k`-ésimo acceso aún no concedido de esa
lane; al conceder uno, los siguientes avanzan de entrada sin cambiar su
petición. Accesos de lanes distintas pueden progresar en paralelo; con más
de una entrada, también accesos a distintos bancos de una lane, según los
puertos y capacidades de fase 3. Se conserva el arbitraje sin cola de
denegaciones.

## Backend

Admite descriptores de uno o varios elementos con la validación de
`validBlockShape()`. Conserva una transacción y un paquete físico en vuelo,
la traducción de todos los fragmentos antes del primer envío, el rechazo
lógico separado de `recvReqRetry` y una única respuesta terminal por
bloque.

## Parámetros

| Parámetro | Unidad y condición |
| --- | --- |
| `memory_max_block_bytes` | Bytes por bloque y buffer; por defecto 64. Debe contener elementos enteros de cada SEW habilitado. |
| `memory_active_blocks` | Debe ser 1. |
| `memory_vrf_inputs_per_lane` | Entradas VLSU por LRF; positivo, por defecto 1. |
| `memory_max_outstanding_packets` | Debe ser 1. |

Se elimina `memory_block_requests`: no existe ya un camino por elemento
seleccionable. Con los valores predeterminados, los ciclos y contadores
difieren de los de fase 3.

## Estadísticas

| Grupo/métrica | Qué mide |
| --- | --- |
| VLSU: `blocks` | Bloques preparados, incluidos los que fallan. |
| VLSU: `splitElementBlocks` | Bloques de un elemento que cruza una frontera. |
| VLSU: `blockBytes` | Distribución de bytes por bloque. |
| VLSU: `blockVrfAccesses` | Distribución de accesos por palabra por bloque. |
| VLSU: `memoryRequests`, `memoryRetries` | Peticiones lógicas aceptadas y rechazadas. |
| VLSU: `memoryTicks` | Desde bloque listo hasta respuesta terminal. |
| VLSU: `vrfGatherTicks`, `vrfScatterTicks` | Fases de lectura/escritura VRF del bloque. |
| VLSU: `vrfReadsGranted`, `vrfWritesGranted` | Concesiones por palabra. |
| Memoria: `fragmentedRequests` | Peticiones con más de un fragmento físico. |
| Memoria: `fragmentsCreated`, `packetsSent`, `portRetries` | Fragmentos, paquetes y reintentos físicos. |

`loadElements` y `storeElements` se cuentan al cerrar cada bloque. Las
esperas del LRF de fase 3 incluyen ahora el tiempo de un acceso publicado
que aún no ocupa una entrada VLSU.

## Referencia normativa

RVV 1.0, manual oficial edición 20240411, [capítulo vectorial][rvv].
Secciones aplicables: *Vector Unit-Stride Instructions*, *Vector Start
Index CSR* y el tratamiento de excepciones en loads/stores vectoriales.
La semántica de la instrucción no cambia: la agrupación es una decisión de
modelo. Queda pendiente contrastar por escrito en esas secciones dos puntos
del contrato: que el índice `firstElement` de un bloque que falla entero
cumple la precisión del trap en stores, y la regla para elementos no
alineados de forma natural (transferencia o *address-misaligned*), que se
conserva como en el baseline: el elemento se transfiere.

## Comprobaciones y pendientes

Comprobaciones realizadas:

- `pre-commit` sobre los archivos modificados: gem5 style, clang-format,
  black, isort y pyupgrade correctos.
- `python3 -m py_compile` de los dos Python: correcto.
- SCons con objetivos explícitos bajo `build/RISCV/`: VLSU, planificador,
  backend, propietario, LRF, Python embebido de `VectorEngine` y
  `configuration`, y parámetros del SimObject: compilación correcta, sin
  avisos del compilador. SCons avisa de que GCC 15.2.1 está fuera de su
  rango declarado.

No se ha enlazado el simulador completo, creado pruebas ni ejecutado
simulaciones.

Para el equipo se sugieren, sin implementar: los tres ejemplos de la tabla
de formación; elemento de 8 bytes desalineado que cruza línea y página;
`vstart` no nulo y rango vacío; grupos `m8` que cruzan registros y
fraccionales; `Retry` lógico y de puerto; fallo de traducción y de acceso en
bloque agrupado y en elemento dividido; `memory_vrf_inputs_per_lane` 1 y 2
con varios bancos; drain con lecturas o escrituras VRF pendientes.

Quedan fuera: bloques o paquetes concurrentes, memoria concurrente con
aritmética, predicación, strided/indexed, `vl2re32.v`/`vs2r.v`,
acciones CPU de 2.4 y cambios en `interface/`.

[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
