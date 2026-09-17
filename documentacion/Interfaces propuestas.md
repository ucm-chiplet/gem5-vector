# Interfaces propuestas para la VPU Ara-like

Este documento concreta las interfaces de comunicación entre los módulos
descritos en [[Cambios en gem5 v25 versión mejorada]]. Es una propuesta de
diseño: no presupone la modificación de las APIs actuales de gem5 ni reutiliza
punteros a las instrucciones RVV 0.7.1 de Vitruvius.

La primera implementación sigue el modelo de Ara: no incluye renombramiento
de registros ni un ROB vectorial y mantiene una única instrucción vectorial
activa. Las estructuras previstas para esas funciones se documentan como una
extensión futura, pero no forman parte del camino de ejecución inicial. Si se
declara inicialmente algún armazón para ellas, permanece inactivo y
desconectado.

## Alcance funcional inicial

El primer objetivo de estas interfaces no es cubrir toda la especificación
RVV, sino ejecutar de extremo a extremo, y en este orden, la secuencia de
referencia definida en [[Cambios en gem5 v25 versión mejorada]]:

1. `vsetvli`: configura `vl`, `vtype`, `SEW` y `LMUL`.
2. `vle32.v`: carga enteros de 32 bits desde memoria al VRF.
3. `vadd.vv`: suma elemento a elemento dos registros vectoriales.
4. `vadd.vx`: suma un registro vectorial y un escalar leído por la CPU.
5. `vse32.v`: almacena en memoria el resultado vectorial.

El baseline sólo exige operandos enteros de 32 bits, accesos de memoria
*unit-stride* y la configuración de LMUL utilizada por el benchmark. Máscaras,
accesos strided/indexed/segmentados, operaciones *fault-only-first*,
reducciones, permutaciones, coma flotante y chaining quedan fuera de este
primer hito. Sus campos pueden permanecer en los mensajes para estabilizar la
API, pero sus valores deben rechazarse como no soportados o mantenerse en su
valor neutro; no deben abrir caminos parcialmente implementados.

El contrato inicial admite una única petición lógica de memoria de un elemento
de 32 bits en curso. Incluye la lectura del VRF para un store o el writeback
de una carga: no se empieza el elemento siguiente hasta cerrar el actual.
El solapamiento de peticiones y el reset con trabajo activo quedan fuera del
primer hito. `drain` permite terminar el trabajo ya admitido.

La responsabilidad mínima de cada instrucción es:

| Instrucción | Ruta principal | Datos mínimos |
| --- | --- | --- |
| `vsetvli` | MinorCPU | `rs1`/AVL, `vtype`, `rd`/`vl` |
| `vle32.v` | VLSU -> VRF | `vd`, base, estado RVV, EEW=32 |
| `vadd.vv` | lanes -> VRF | `vs1`, `vs2`, `vd`, estado RVV |
| `vadd.vx` | lanes -> VRF | `vs2`, `vd`, valor de `rs1`, estado RVV |
| `vse32.v` | VLSU -> memoria | `vs3`, base, estado RVV, EEW=32 |

### Criterio de validación

La validación inicial es exclusivamente funcional. Debe comprobar que:

- el programa RVV arranca y termina correctamente en modo SE;
- `vsetvli` actualiza la configuración vectorial esperada;
- `vle32.v` lee los datos correctos desde memoria;
- `vadd.vv` y `vadd.vx` producen los valores esperados;
- `vse32.v` escribe el resultado final en la dirección prevista;
- el resultado almacenado coincide con el resultado de referencia.

El benchmark debe devolver un código distinto de cero si falla cualquiera de
estas comprobaciones. No se exige todavía validar ciclos, rendimiento,
conflictos de bancos, utilización de unidades ni solapamiento entre comandos.

## Principios de diseño

- `VectorCommand` es la única representación de una instrucción vectorial que
  cruza la frontera entre `MinorCPU` y la VPU.
- La VPU no recibe `StaticInst`, `DynInst` ni realiza una segunda
  decodificación de la instrucción.
- Todos los mensajes de la VPU incluyen una identidad estable de comando;
  las tareas internas incluyen además una identidad de tarea.
- Las operaciones sobre registros usan en el baseline una referencia al grupo
  arquitectónico y un rango explícito de bytes. Los rangos de bits se reservan
  para la futura ampliación de máscaras.
- Sólo puede haber un comando ejecutándose en el backend. El siguiente comando
  espera en la FIFO hasta recibir la finalización del anterior.
- La aceptación de un comando (`accepted`) y su finalización (`completed`) son
  eventos diferentes.
- La falta temporal de capacidad (`stall`) y el rechazo permanente de un
  comando (`rejected`) son resultados diferentes. Sólo `stall` se reintenta.
- Las interfaces temporizadas usan backpressure explícito (`ready/valid`, o
  `retry` en la frontera con la memoria de gem5).

```text
MinorCPU
   |  VectorCommand + grant/accepted/completed
   v
Frontend VPU: admisión, FIFO y sequencer
   |  ArithmeticTask / MemoryTask, con UnitTask
   v
Lanes y VLSU <--> VRF distribuido <--> enlace ideal
   |
   |  VectorMemoryRequest / VectorMemoryResponse
   v
Memoria y traducción de gem5

Extensiones: SLDU, MASKU, interconexión temporizada, renombramiento y ROB
```

## Tipos compartidos

Los siguientes tipos especifican contratos de diseño; su forma final puede
adaptarse a las convenciones C++ de gem5. Los bloques no son cabeceras
compilables independientes: omiten includes y usan tipos definidos en otras
secciones. Una declaración documental no implica que exista implementación.

En el árbol actual, `common/` e `interface/` contienen los tipos básicos,
`VectorCommand`, `VectorCompletion` y los extremos abstractos de admisión y
finalización. Las tareas especializadas, fragmentos de lane, accesos al VRF,
mensajes de memoria y estados internos descritos aquí están pendientes de
implementación. También lo están los validadores completos y los módulos
ejecutables. Esta revisión modifica documentación, no esas cabeceras.

```cpp
struct CommandKey
{
    uint64_t commandId;
    ContextID contextId;
};

struct VectorRegRef
{
    uint8_t firstReg;
    uint8_t regCount;
};

enum class VectorLmul : int8_t
{
    Mf8 = -3,
    Mf4 = -2,
    Mf2 = -1,
    M1 = 0,
    M2 = 1,
    M4 = 2,
    M8 = 3,
    Invalid = 127,
};

struct VectorConfig
{
    uint32_t vl;
    uint32_t vstart;
    uint16_t sewBits;
    VectorLmul lmul;
    bool masked;
    bool tailAgnostic;
    bool maskAgnostic;
};

enum class CompletionStatus : uint8_t
{
    Success,
    MemoryFault,
    IllegalInstruction,
    InternalError,
    Cancelled,
};

// Reservado para la ampliación futura con renombramiento.
struct PhysicalRegRef
{
    uint32_t version;
    uint8_t firstReg;
    uint8_t regCount;
};

struct ByteRange
{
    uint32_t offset;
    uint32_t size;
};

using TaskId = uint32_t;

inline constexpr TaskId InvalidTaskId =
    std::numeric_limits<TaskId>::max();

enum class VectorUnitClass : uint8_t
{
    Invalid,
    Lanes,
    Vlsu,
};

struct TaskKey
{
    CommandKey command;
    TaskId taskId;
};

struct ElementRange
{
    uint32_t firstElement;
    uint32_t elementCount;
};

struct UnitTask
{
    TaskKey key;
    VectorUnitClass unit;
    ElementRange elements;
};

struct ScalarResult
{
    RegIndex destination;
    RegVal value;
};

struct FaultInfo
{
    Fault fault;
    std::optional<Addr> address;
    std::optional<uint32_t> elementIndex;
};

struct VectorCompletion
{
    CommandKey command;
    CompletionStatus status;
    std::optional<ScalarResult> scalarResult;
    uint32_t finalVstart;
    std::optional<FaultInfo> fault;
};
```

`CompletionStatus` distingue éxito, fault de memoria, instrucción ilegal,
error interno y cancelación.

### Semántica e invariantes

Estos tipos no son sólo contenedores de datos: fijan la identidad, la
propiedad y el alcance de cada operación entre módulos. Deben viajar por valor
o mediante referencias inmutables; ningún consumidor debe modificar un objeto
que otro módulo pueda observar. `CpuVectorInterface` crea las identidades, el
backend las propaga y el sequencer las descarta al finalizar el comando. Una
futura configuración con ROB podrá conservarlas hasta el retiro.

- `CommandKey` vive en `VectorCommand`, `TaskKey`, peticiones de memoria y
  `VectorCompletion`. `CpuVectorInterface` asigna la identidad y MinorCPU la
  incorpora al construir el comando. `CommandQueue`, `AraSequencer`, las
  unidades y el backend de memoria la propagan hasta que `VectorCompletion`
  vuelve a `MinorCPU`. Su función es
  correlacionar todos esos mensajes con una única instrucción. `commandId` es
  su número de secuencia y sólo debe ser único dentro de un contexto;
  `contextId` identifica el hilo o contexto de gem5 que la posee. La pareja
  `(contextId, commandId)` evita que, por ejemplo, `(3, 41)` y `(7, 41)` se
  confundan. En el baseline un único contexto hace que `contextId` sea
  constante, pero se conserva para no cambiar la interfaz al añadir hilos.

- `VectorRegRef` vive en el comando y en las tareas que el sequencer entrega a
  `TaskDistributor`, `AraVLSU` y las lanes. Identifica un grupo de registros
  arquitectónicos: `firstReg` es su inicio y `regCount` el número de registros
  contenedores que requiere LMUL/EMUL.
  Viaja hacia `LaneRegisterFile` para seleccionar el grupo leído o escrito.
  Existe porque el baseline Ara-like no renombra registros y necesita una
  referencia estable, sin introducir versiones físicas innecesarias.

- `PhysicalRegRef` sólo vive en la ampliación futura con renombramiento.
  El renombrador la crearía, la enviaría con las tareas a VRF y
  `ReadinessTable`, y el ROB la devolvería a la free-list al retirar el
  comando. Añade `version` a un grupo para distinguir asignaciones sucesivas y
  resolver dependencias WAR/WAW; no se instancia ni se consulta en el baseline.

- `ByteRange` vive dentro de tareas, accesos al VRF, paquetes de interconexión
  y mensajes de carga. Lo generan el sequencer, `TaskDistributor` o `AraVLSU`,
  y lo consumen las lanes, `LaneRegisterFile` y la VLSU al leer o escribir.
  Expresa el intervalo semiabierto `[offset, offset + size)` dentro de un grupo
  de registros. Existe para identificar exactamente los bytes de una operación
  parcial; debe caber en `VectorRegRef` y no puede estar vacío.

- `TaskId` es un identificador asignado por `AraSequencer` y sólo necesita ser
  único dentro del `CommandKey` al que pertenece. El valor reservado como
  inválido no se asigna y un identificador no se reutiliza mientras siga viva
  una tarea del mismo comando.

- `TaskKey` combina el `CommandKey` padre y el `TaskId`. Es la identidad que
  conservan `TaskDistributor`, las lanes, `AraVLSU`, los accesos al VRF y los
  paquetes de interconexión hasta devolver la finalización al sequencer. La
  pareja completa, y no uno de sus campos por separado, identifica una tarea.

- `ElementRange` expresa el intervalo semiabierto
  `[firstElement, firstElement + elementCount)`. No puede estar vacío, la suma
  no puede desbordar y el intervalo de una tarea ejecutable debe quedar dentro
  de `[vstart, vl)` del comando padre. Cuando `vstart >= vl`, el comando tiene
  un rango de ejecución vacío y el sequencer no crea tareas para elementos.

- `UnitTask` vive sólo dentro del backend VPU. `AraSequencer` la crea al partir
  un `VectorCommand` y la envía, dentro de una tarea especializada, a
  `TaskDistributor`, `AraVLSU` u otra unidad. Contiene únicamente identidad,
  unidad receptora e intervalo lógico de elementos; no contiene operandos,
  opcode ni rangos de registros. `ArithmeticTask` añade su
  `destinationRange`, mientras que `MemoryTask` añade un `dataRange` que es
  destino para una carga y fuente para un store. Así `UnitTask` no atribuye
  una semántica de destino incorrecta a las tareas de store.

- `VectorCompletion` vive en la frontera de salida VPU--CPU. `AraSequencer` la
  crea una vez agregadas todas las tareas y la envía por `CpuVectorInterface` a
  `MinorCPU::Execute/Commit`. Existe para cerrar el comando, liberar el estado
  en vuelo y transportar resultado escalar, `vstart` o excepción. El resultado
  escalar sólo existe cuando `scalarResult` contiene un valor y `fault` sólo
  debe existir en estados de fallo arquitectónico. Las combinaciones válidas
  del baseline se fijan en la tabla de finalización siguiente.

En las peticiones al VRF y de memoria, `registerRef` designa un
`VectorRegRef` en el baseline y un `PhysicalRegRef` cuando se habilita el
renombramiento. Cada petición conserva esa referencia y su rango de bytes;
no se añade una versión física ficticia al modo sin renombramiento.
En memoria, `TaskKey` y `requestId` identifican la tarea y su subpetición en
ambos modos, mientras que la referencia de registro identifica dónde leer o
escribir. La VLSU conserva la referencia original hasta resolver la respuesta,
sin volver a consultar la RAT para una petición ya emitida.

`requestId` aplica el mismo patrón dentro de memoria: `AraVLSU` lo crea para
cada subpetición de un `TaskKey`, `VectorMemoryBackend` lo devuelve en la
respuesta y la VLSU lo usa para asociarla explícitamente, sin depender del
orden de llegada. Una ampliación posterior podrá formalizar `RequestKey` como
la pareja `(TaskKey, requestId)`. Hasta entonces, las firmas deben transportar
el `TaskKey` completo, incluido el `CommandKey` con su `contextId`.

### Representación de datos e intercambio

```cpp
using LaneId = uint32_t;
using BankId = uint32_t;
using ByteBuffer = std::vector<uint8_t>;
using ByteEnable = std::vector<uint8_t>;

enum class TransferResult : uint8_t
{
    Accepted,
    Retry,
};
```

`LaneId` y `BankId` son índices desde cero, acotados por la geometría. No se
usan valores ficticios de lane para representar a la VLSU.

`ByteBuffer[i]` corresponde al byte de menor a mayor dirección o
desplazamiento del rango transportado. Un `ByteEnable` tiene una entrada
por byte, exclusivamente 0 o 1. En las transferencias de elementos completos
del baseline todas son 1; al mapearlas a una palabra se deshabilitan los bytes
exteriores al rango. Las máscaras de predicación RVV no se confunden con
estas máscaras de escritura.

En enlaces internos, `TransferResult` es una respuesta síncrona a un intento
de entrega de tarea, acceso o petición. `Accepted` significa que el receptor
ha copiado el descriptor y sus buffers; no que haya ejecutado la operación.
`Retry` no transfiere propiedad ni causa efectos: el emisor conserva el mismo
mensaje e identidad y puede reintentarlo en una oportunidad posterior. Este
contrato no sustituye `GrantResult` en la frontera CPU--VPU.

Tras aceptar un mensaje, el receptor debe conservarlo hasta emitir su única
respuesta terminal. Las respuestas y finalizaciones se entregan a un estado
de recepción reservado al aceptar la petición; no se descartan ni exigen
reenviar una operación ya aceptada. Si ese espacio no existe, se devuelve
`Retry` antes de aceptar. No se guardan referencias a buffers temporales.

### Progreso y reactivación tras Retry

Un emisor con mensajes internos no aceptados programa una evaluación en el
siguiente ciclo de su reloj. Mantiene como máximo un evento de reintento
pendiente por emisor; ese evento atiende el trabajo pendiente sin crear
copias nuevas de mensajes o identidades. Si sigue recibiendo `Retry`, programa
la siguiente evaluación. No depende de que Minor tenga trabajo escalar.

Tras `Accepted` se deja de reintentar ese mensaje. El receptor conserva la
obligación de responder y la respuesta reactiva al consumidor si habilita
trabajo nuevo. Un módulo puede dormir cuando sólo espera respuestas aceptadas;
si queda trabajo listo, debe mantener programada su evaluación. Un callback
inmediato no debe provocar un bucle de reintentos en el mismo tick.

Minor aplica el mismo principio a `GrantStatus::Stall` mientras pueda iniciar
un offload. Al comenzar drain deja de solicitar grants nuevos y aplica la
política de descarte o finalización de la sección de drain conjunto.

Esta regla es funcional, no una especificación de throughput. El retry de un
puerto timing de gem5 es distinto: después de un `sendTimingReq` fallido, el
backend espera `recvReqRetry`; no sondea el puerto cada ciclo. Una vez recibido
ese aviso puede reintentar el paquete conservado, sin que la VLSU vuelva a
emitir la petición lógica.

### Invariantes de finalización CPU--VPU

| `CompletionStatus` | `fault` | `scalarResult` en el baseline | `finalVstart` | Uso |
| --- | --- | --- | --- | --- |
| `Success` | Ausente | Ausente | 0 | Todas las tareas y escrituras han terminado; también rango vacío. |
| `MemoryFault` | `FaultInfo` con `fault != NoFault`, dirección y elemento causante | Ausente | Índice del elemento que falla | La VLSU ha cerrado la petición fallida sin iniciar elementos posteriores. |
| `IllegalInstruction` | No se emite como finalización del baseline | Ausente | No aplicable | Un descriptor no soportado se rechaza antes de `accepted`; Minor construye la excepción. |
| `InternalError` | No se emite como finalización normal | Ausente | No aplicable | Invariante rota o identidad desconocida: aserción o error fatal del simulador. |
| `Cancelled` | Reservado, sin contrato activo | Ausente | No aplicable | No hay cancelación por drain ni reset de trabajo activo en este hito. |

`ScalarResult` permanece reservado para futuras instrucciones; `vsetvli`
produce su resultado escalar en MinorCPU. Un fault no retira la instrucción
como éxito: Minor aplica la excepción y el `vstart` recibido según su camino
de faults. Las cabeceras actuales todavía no comprueban esta tabla.

Una identidad desconocida, una finalización duplicada o un rango fuera del
comando son errores internos, no faults de memoria del programa simulado.
Los identificadores reservados como inválidos nunca se asignan. Los contadores
no deben desbordar ni volver a cero: su agotamiento es un error diagnosticado.

## Configuración global de la VPU

`VectorConfig` es el estado RVV de una instrucción. `VpuParameters` describe
la configuración del coprocesador y es compartida por sus módulos:

```cpp
struct VpuParameters
{
    VrfGeometry vrf;
    std::vector<VectorLmul> supportedLmuls;
    uint32_t commandQueueEntries;
    uint32_t laneTaskEntries;
    uint32_t operandBufferEntriesPerSource;
    uint32_t resultBufferEntries;
};
```

`VrfGeometry` se define en la sección de `AddressMapper`. Los parámetros del
SimObject `VectorEngine` son el origen de estos valores; la instancia C++ de
`VectorEngine` los valida al construirse y conserva la configuración. Los
módulos reciben vistas inmutables válidas durante toda su vida, sin mantener
copias modificables independientes. No cambia con trabajo activo.

| Campo | Unidad y consumidor | Restricción del baseline |
| --- | --- | --- |
| `vrf` | Geometría común para mapper, VRF, distribuidor y VLSU | Cumple las restricciones de mapeo y coincide con VLEN de Minor. |
| `supportedLmuls` | Conjunto de factores aceptados por admisión | No vacío, sin duplicados ni `Invalid`; cada factor permite al menos un elemento de 32 bits. |
| `commandQueueEntries` | Comandos, incluyendo cabeza activa y reservas | Al menos 1; corresponde a `queueDepth` en la fórmula de admisión. |
| `laneTaskEntries` | Fragmentos aceptados por lane, incluidos los activos | Al menos 1. |
| `operandBufferEntriesPerSource` | Buffers de fragmento por fuente vectorial y lane | Al menos 1 para cada una de las dos fuentes; cada buffer aloja hasta una palabra de lane. |
| `resultBufferEntries` | Resultados de fragmento pendientes de writeback por lane | Al menos 1; cada buffer aloja hasta una palabra de lane. |

Una suma vectorial puede reservar una entrada de cada fuente y una de resultado
sin exigir que todas las tareas aceptadas progresen simultáneamente. La
reserva de espacio para respuestas no se comparte de forma que una fuente
ocupe toda la capacidad e impida recibir la otra. En `vadd.vx`, la segunda
fuente procede del escalar y no necesita lectura del VRF.

`VectorEngine` distribuye configuración y valida compatibilidad, pero no
decide operaciones ni vuelve a decodificar. La FIFO gestiona sus créditos;
cada lane gestiona sus buffers. La VLSU mantiene el límite ya fijado de un
elemento lógico en curso. Los valores de VLEN, LMUL y capacidades se declaran
en la configuración de ejecución; este contrato no inventa un benchmark ni
fija latencias, frecuencia o número de puertos del modelo temporal futuro.

## Decode de MinorCPU

**Conexiones directas.** Recibe instrucciones de la entrada de Decode de
`MinorCPU` y la configuración de offload del propio CPU. Envía la instrucción
clasificada a `MinorCPU::Execute` o a la ruta nativa de ejecución.

### Recibe

- Instrucción RISC-V decodificada.
- `vectorOffloadEnabled` y disponibilidad del coprocesador.

### Envía

- Una macroinstrucción RVV intacta a `MinorCPU::Execute` cuando se debe
  descargar a la VPU.
- Las microoperaciones nativas de gem5 cuando el offload está desactivado.
- Una clasificación explícita de destino:

```text
CPU_SCALAR | CPU_VECTOR_CONFIG | VPU_OFFLOAD | NATIVE_VECTOR
```

La clasificación es un atributo de la macroinstrucción que Decode entrega a
`MinorCPU::Execute`; no es un mensaje independiente. Execute y Commit la usan
para decidir si la instrucción sigue el issue normal, modifica la configuración
RVV o cruza la interfaz CPU--VPU.

| Clasificación | Ruta resumida |
| --- | --- |
| `CPU_SCALAR` | Execute -> issue escalar; no entra en la VPU |
| `CPU_VECTOR_CONFIG` | Execute/Commit -> estado RVV; sin `VectorCommand` |
| `VPU_OFFLOAD` | Commit -> `CpuVectorInterface` -> `CommandQueue` |
| `NATIVE_VECTOR` | Execute -> microoperaciones vectoriales de gem5 |

En el baseline, `vsetvli` pertenece a `CPU_VECTOR_CONFIG`: MinorCPU lee AVL de
`rs1`, valida el inmediato `vtype`, calcula `vl`, actualiza el estado RVV y
escribe `vl` en el destino escalar. No se despacha como comando de ejecución a
la VPU. `vle32.v`, `vadd.vv`, `vadd.vx` y `vse32.v` pertenecen a `VPU_OFFLOAD`.
Las demás instrucciones vectoriales quedan fuera del primer hito.

La decisión se toma una sola vez en Decode y viaja con la macro hasta Execute;
Commit no vuelve a decodificar el opcode. Para el primer hito, sólo
`vle32.v`, `vadd.vv`, `vadd.vx` y `vse32.v` reciben `VPU_OFFLOAD`; si el
offload está desactivado, esas instrucciones se clasifican como
`NATIVE_VECTOR`.

### Descripción decodificada conservada en CPU

`DecodedVectorOp` pertenece exclusivamente al lado CPU. Viaja asociado a la
macroinstrucción desde Decode hasta Execute; no se entrega a la VPU.

```cpp
enum class VectorExecutionClass : uint8_t
{
    CpuScalar,
    CpuVectorConfig,
    VpuOffload,
    NativeVector,
};

struct ScalarOperandRef
{
    uint32_t sourceOperandIndex;
};

struct DecodedArithmeticOp
{
    ArithmeticOperation operation;
    RegIndex destination;
    RegIndex vectorSource;
    std::variant<RegIndex, ScalarOperandRef> secondOperand;
};

struct DecodedMemoryOp
{
    MemoryDirection direction;
    RegIndex dataReg;
    ScalarOperandRef base;
    uint16_t elementWidthBits;
};

struct DecodedVectorOp
{
    VectorExecutionClass executionClass;
    bool masked;
    std::optional<std::variant<DecodedArithmeticOp, DecodedMemoryOp>> payload;
};
```

Los valores del enum corresponden a las cuatro clases de la tabla anterior.
`payload` existe sólo para `VpuOffload`. Las otras clases conservan sus rutas
de ejecución CPU; en particular, esta estructura no sustituye la semántica
de `vsetvli` en el lado CPU.

`masked` conserva la predicación decodificada de la instrucción y se copia a
`VectorConfig.masked` al construir el comando. No se deduce de los CSR RVV.
Permite rechazar comandos enmascarados en admisión sin volver a decodificar;
no activa soporte de máscaras. Para clases sin payload se mantiene neutro.

Los campos `RegIndex` son números arquitectónicos vectoriales entre 0 y 31,
no índices de las tablas de operandos ni grupos ya dimensionados.
`ScalarOperandRef.sourceOperandIndex` sí indexa la tabla de fuentes de la
instrucción; debe ser menor que `numSrcRegs()` y designar un registro entero.
Execute usa las APIs de operandos de gem5 y el scoreboard para resolverlo,
sin interpretarlo como un número de registro escalar. Esta referencia no
contiene aún un `RegVal` capturado.

Decode obtiene la semántica una sola vez a partir de la instrucción
decodificada y construye el payload. Para el baseline, aritmética implica
`Add`, `SameWidth` y `NotApplicable`; memoria implica unit-stride,
`elementWidthBits=32`, un campo y sin fault-only-first. Esos atributos fijos
no necesitan campos duplicados. Otras operaciones no reciben un payload de
offload válido.

Cuando puede capturar operandos, Execute lee la configuración RVV y resuelve
las referencias escalares. A partir de los números vectoriales y LMUL/EMUL
construye `VectorRegRef`; después forma el payload de `VectorCommand` y añade
identidad, PC y `requestorId`. Los reintentos reutilizan esa captura. Commit
no vuelve a leer bits de instrucción para deducir operación o operandos.

## Minor Execute y Commit

`Minor Execute` y `Commit` son etapas de la CPU, no módulos de la VPU. Su
función es retener la instrucción hasta que sea seguro enviarla y cerrar su
ejecución cuando la VPU responda.

**Conexiones directas.** Dentro de la CPU, recibe la macroinstrucción de
`Decode` y consulta el scoreboard y el estado RVV de `MinorCPU`. En la frontera
CPU--VPU, envía peticiones y comandos a `CpuVectorInterface` y recibe de ella
`grant`, `accepted` y `completed`, que son respuestas originadas en la VPU.

### Recibe de la CPU (`Decode` y estado interno)

- Macroinstrucción desde Decode.
- Estado del scoreboard de MinorCPU.
- Estado arquitectónico RVV (`vl`, `vtype`, `vstart`).

`Decode` ya ha marcado la instrucción como `CPU_VECTOR_CONFIG`, `VPU_OFFLOAD` o
otra clase. Para `vsetvli`, Execute actualiza directamente la configuración RVV
y no atraviesa la VPU. Para las cuatro instrucciones del baseline, comprueba
dependencias escalares y lee de `MinorCPU` la base de memoria o el escalar de
`vadd.vx` que se copiará al `VectorCommand`.

### Recibe desde la VPU a través de `CpuVectorInterface`

- `grant`, `stall` o `rejected` para la consulta de admisión.
- `accepted(CommandKey)` cuando `CommandQueue` ya posee el comando.
- `completed(VectorCompletion)` cuando `AraSequencer` ha terminado todas las
  tareas o ha producido un fault.

### Envía a la CPU (`MinorCPU`)

- Reserva y liberación de destinos escalares en el scoreboard.
- Alta y actualización de la instrucción en `inFlightInsts`.
- Actualización del resultado escalar, `vstart` o fault recibidos en
  `completed`.

### Envía a la VPU mediante `CpuVectorInterface`

- `requestGrant(command)`, para preguntar si `CommandQueue` puede aceptar el
  comando. La consulta es inmutable y no transfiere su propiedad a la VPU.
- `dispatch(grantToken, VectorCommand)`, con la macro RVV y el estado capturado
  por la CPU. `CpuVectorInterface` lo reenvía al frontend VPU.

La CPU no envía `StaticInst` ni `DynInst` a la VPU, ni vuelve a decodificar la
instrucción después de `Decode`.

La instrucción vectorial mantiene un estado local:

```text
WAIT_DEPENDENCIES -> WAIT_GRANT -> DISPATCHED -> ACCEPTED -> COMPLETED
```

Las transiciones significan lo siguiente:

1. `WAIT_DEPENDENCIES`: Decode ha identificado un posible offload y Execute
   conserva la macro en `inFlightInsts`. Mientras falte un registro escalar o
   la instrucción anterior de Minor, no se lee el operando ni se contacta con
   la VPU.
2. `WAIT_GRANT`: las dependencias escalares están listas. Execute consume la
   descripción decodificada y captura operandos, obtiene un `CommandKey` de
   `CpuVectorInterface`, construye el `VectorCommand` y envía `requestGrant`.
   Un `stall` no cambia este estado y se vuelve a intentar en un ciclo
   posterior. Un `rejected` es permanente y se convierte en una instrucción
   ilegal; no se reintenta.
3. `DISPATCHED`: la interfaz CPU ha recibido `grant` y Execute ha enviado
   `dispatch(grantToken, command)`. La instrucción espera confirmación de que
   el comando fue almacenado en la FIFO de la VPU.
4. `ACCEPTED`: llega `accepted` desde `CommandQueue` a través de
   `CpuVectorInterface`. La VPU ya es propietaria del comando; Minor no lo
   vuelve a enviar, pero mantiene la entrada en vuelo.
5. `COMPLETED`: llega `VectorCompletion` desde la VPU. Minor actualiza el
   resultado escalar o `vstart` y cierra el estado de offload. Con éxito libera
   las dependencias y retira la instrucción; con fault aplica su camino de
   excepción, sin retirarla como una instrucción ejecutada con éxito.

En `vsetvli` no se recorren estos estados de offload: Execute actualiza
`vl`/`vtype`, escribe el `vl` escalar y completa la instrucción localmente.
Para `vle32.v`, `vadd.vv`, `vadd.vx` y `vse32.v`, mantener la entrada hasta
`COMPLETED` garantiza que los datos de la VPU y los faults lleguen antes de que
la CPU continúe con instrucciones dependientes.

El estado local de Minor conserva la entrada de `inFlightInsts`, la fase de
offload, el `VectorCommand` una vez construido y el token mientras no se haya
consumido. La asociación CPU entre `CommandKey` y la instrucción nunca cruza
la frontera con la VPU. Ante `Stall`, no se reasigna identidad ni se vuelven
a capturar operandos. Tras `accepted`, Minor puede liberar su copia del
descriptor, pero conserva la asociación hasta `completed`.

Las llamadas de entrega y sus callbacks pueden ser síncronos. El emisor
prepara su estado pendiente antes de invocarlas, para que `accepted` o una
respuesta inmediata encuentren una identidad válida; si recibe `Retry`,
conserva el mensaje como no aceptado. El contrato no presupone un ciclo
intermedio entre entrega y respuesta.

### Estado privado de offload en MinorCPU

`MinorVectorState` es propiedad de Execute y está asociado a una entrada de
`inFlightInsts`. No forma parte de los tipos compartidos de la VPU:

```cpp
enum class MinorVectorPhase : uint8_t
{
    WaitDependencies,
    WaitGrant,
    Dispatched,
    Accepted,
    Completed,
};

struct MinorVectorState
{
    MinorDynInstPtr inst;
    MinorVectorPhase phase;
    DecodedVectorOp decoded;
    std::optional<CommandKey> commandKey;
    std::optional<VectorCommand> command;
    std::optional<GrantToken> grantToken;
    std::optional<VectorCompletion> completion;
};
```

`inst` mantiene la asociación con la instrucción del pipeline. Execute posee
este estado; la instrucción no contiene un puntero propietario a sí misma.
Puede indexarlo por su identidad dinámica y conservar además una asociación
de `CommandKey` al estado para los callbacks. Ninguna de esas referencias
CPU se entrega a las unidades vectoriales.

| Fase | Campos y condición |
| --- | --- |
| `WaitDependencies` | `inst` y `decoded` válidos; sin comando, clave, token ni finalización. No se han capturado valores escalares. |
| `WaitGrant` sin token | Clave y comando capturados e inmutables. `Stall` conserva ambos y programa otro intento mientras no se esté drenando. |
| `WaitGrant` con token | La reserva ya se concedió. Se prepara el estado de despacho y se consume el token; no se vuelve a llamar a `requestGrant`. |
| `Dispatched` | Clave y comando válidos, esperando `accepted`. El token se consume en la entrega y no queda disponible para un segundo despacho. |
| `Accepted` | Clave e instrucción retenidas; sin token. Se puede liberar la copia del comando. No se puede descartar por squash o drain. |
| `Completed` | Clave e instrucción retenidas y una finalización recibida. Minor debe procesar el resultado o fault antes de liberar la asociación. |

La clave de `command`, `grantToken` y `completion`, cuando existan, debe
coincidir con `commandKey`. En `WaitGrant`, `Rejected` cierra el intento por
el camino de excepción local de Minor: no se fabrica `VectorCompletion` ni
se espera una respuesta de una orden no aceptada. La identidad consumida
no se reutiliza. Un squash previo a tener reserva puede eliminar el estado
mediante el camino normal de descarte de Minor y su limpieza de dependencias.

La concesión se solicita en commit, cuando ya no existe una instrucción
anterior capaz de anularla. Una reserva concedida se consume incluso si
comienza drain. Un comando despachado o aceptado conserva su instrucción hasta
completar; las instrucciones posteriores sí pueden descartarse. Estas reglas
deben integrarse en los caminos de descarte de Minor, no sólo en el callback.

Antes de `dispatch`, el estado ya permite recibir `accepted`; antes de admitir
ejecución, permite recibir `completed`. Al volver de una llamada síncrona no
se sobrescribe una fase que el callback haya avanzado ni se accede a un estado
ya liberado. La finalización se procesa una vez: con éxito se retira, con fault
se actualiza `vstart` y se sigue el camino de excepción. Sólo después se libera
la asociación con `CommandKey` y la referencia retenida a la instrucción.

## CpuVectorInterface

Esta interfaz encapsula la frontera CPU--VPU. `MinorCPU` decodifica la
instrucción y extrae su semántica y operandos; `CpuVectorInterface` asigna la
identidad, valida el descriptor y lo transporta sin recibir `StaticInst` ni
`DynInst`.

**Conexiones directas.** Su extremo CPU es `MinorCPU::Execute/Commit` y su
extremo VPU es el control de admisión y `CommandQueue`. No se conecta a lanes,
VRF ni memoria.

### Recibe desde MinorCPU

- Una consulta de admisión con un `VectorCommand` inmutable.
- Un `VectorCommand` para despacho.
- Solicitud de drain; reset únicamente sin trabajo pendiente.

### Envía a MinorCPU

- `grant`, `stall` o `rejected`.
- `accepted(CommandKey)`.
- `completed(VectorCompletion)`.

### Envía al frontend VPU

- Solicitud de admisión con acceso inmutable al comando.
- Comando validado y su información de contexto.

La API recomendada es:

```cpp
enum class GrantStatus : uint8_t
{
    Granted,
    Stall,
    Rejected,
};

enum class RejectionReason : uint8_t
{
    InvalidIdentity,
    InvalidVectorConfig,
    InvalidRegisterGroup,
    InvalidPayload,
    UnsupportedOperation,
    UnsupportedConfiguration,
    DuplicateCommand,
};

struct GrantToken
{
    uint64_t reservationId;
    CommandKey command;
};

struct GrantResult
{
    GrantStatus status;
    std::optional<GrantToken> token;
    std::optional<RejectionReason> rejectionReason;
};

CommandKey allocateCommandKey(ContextID contextId);
GrantResult requestGrant(const VectorCommand &command);
void dispatch(GrantToken token, const VectorCommand &command);

void accepted(CommandKey command);
void completed(const VectorCompletion &completion);
```

`allocateCommandKey` mantiene un contador monotónico por contexto. La identidad
completa es siempre `(contextId, commandId)` y los identificadores consumidos
no se reutilizan aunque el comando resulte rechazado.

`GrantResult` distingue `Granted`, `Stall` y `Rejected`. `Stall` indica falta
temporal de capacidad y permite reintentar; `Rejected` indica un descriptor
inválido o no soportado y no debe reintentarse. Cuando el resultado es
`Granted`, contiene un `GrantToken` opaco creado por la VPU. Sólo
`Rejected` contiene `rejectionReason`; `Granted` contiene un token y `Stall`
no contiene ninguno de los dos.

El `GrantToken` reserva la capacidad concedida entre `requestGrant` y
`dispatch`, evitando una carrera entre consultar la capacidad y almacenar el
comando. Contiene el `CommandKey` asociado y un identificador de reserva, sólo
puede consumirse una vez y no puede utilizarse para otro comando.

La interfaz se divide en dos extremos C++ no propietarios: un
`VpuCommandEndpoint`, que recibe `requestGrant` y `dispatch`, y un
`CpuCompletionEndpoint`, que recibe `accepted` y `completed`.

## VectorCommand

El descriptor se considera inmutable desde la llamada a `requestGrant`. El
`dispatch` asociado debe presentar el mismo comando que se utilizó al conceder
el token. Después de `accepted`, la VPU conserva su propia copia. En el baseline
los registros se mantienen como referencias arquitectónicas; una
implementación futura puede traducirlas a versiones físicas dentro de la VPU.

### Contenido

`VectorCommand` no contiene un opcode por cada mnemónico RVV. La instrucción se
normaliza en una operación semántica y un payload tipado. Así, `vadd.vv` y
`vadd.vx` comparten `ArithmeticOperation::Add`; se distinguen por el tipo de su
segundo operando. De la misma forma, `vle32.v` y `vse32.v` comparten el
descriptor de memoria y se distinguen mediante `MemoryDirection`.

```cpp
enum class ArithmeticOperation : uint8_t
{
    Invalid,
    Add,
};

enum class ElementWidthMode : uint8_t
{
    SameWidth,
    Widening,
    Narrowing,
};

enum class ElementSignedness : uint8_t
{
    NotApplicable,
    Signed,
    Unsigned,
};

using ArithmeticOperand =
    std::variant<VectorRegRef, RegVal, int64_t>;

struct ArithmeticCommand
{
    ArithmeticOperation operation;
    ElementWidthMode widthMode;
    ElementSignedness signedness;
    VectorRegRef destination;
    VectorRegRef vectorSource;
    ArithmeticOperand secondOperand;
};
```

La alternativa activa de `ArithmeticOperand` representa respectivamente una
forma vector--vector, vector--escalar o vector--inmediato. No se almacena además
una forma `vv`, `vx` o `vi`, porque sería información duplicada susceptible de
contradecir el tipo real del operando.

El direccionamiento de memoria sigue la misma regla:

```cpp
enum class MemoryDirection : uint8_t
{
    Invalid,
    Load,
    Store,
};

enum class MemoryOrdering : uint8_t
{
    NotApplicable,
    Ordered,
    Unordered,
};

struct UnitStrideAddress
{};

struct StridedAddress
{
    RegVal stride;
};

struct IndexedAddress
{
    VectorRegRef index;
    MemoryOrdering ordering;
};

using MemoryAddressing = std::variant<
    UnitStrideAddress,
    StridedAddress,
    IndexedAddress>;

struct MemoryCommand
{
    MemoryDirection direction;
    VectorRegRef dataReg;
    Addr base;
    MemoryAddressing addressing;
    uint16_t elementWidthBits;
    uint8_t fieldCount;
    bool faultOnlyFirst;
};
```

`dataReg` es el grupo de destino de una carga o el grupo fuente de un store.
La alternativa activa de `MemoryAddressing` determina el patrón y evita
combinaciones inconsistentes entre un modo, un stride y un registro índice.

El descriptor que cruza la frontera queda formado por:

```cpp
using VectorCommandPayload =
    std::variant<ArithmeticCommand, MemoryCommand>;

struct VectorCommand
{
    CommandKey command;
    Addr pc;
    VectorConfig config;
    VectorCommandPayload payload;
    RequestorID requestorId;
};
```

`ContextID` forma parte de `CommandKey`; `requestorId` identifica al
solicitante de memoria. El soporte inicial en modo SE no necesita transportar
privilegio, virtualización, ASID ni VMID. Esos metadatos se incorporarán al
contrato cuando se defina su semántica de traducción.

La unidad receptora se deriva del payload: un `ArithmeticCommand` se dirige a
las lanes y un `MemoryCommand` a la VLSU. No se almacena un campo `unit` en
`VectorCommand`, porque duplicaría información y podría contradecir el payload.

La frontera proporciona validación estructural y validación del subconjunto
implementado. La primera rechaza descriptores incoherentes; la segunda produce
`rejected` para una operación bien formada que la VPU aún no soporte.

En el baseline sólo se generan los siguientes subconjuntos:

- `vle32.v`: `MemoryCommand` de carga, `dataReg=vd`, base virtual,
  `UnitStrideAddress`, EEW de 32 bits y estado RVV.
- `vadd.vv`: `ArithmeticCommand` de suma, `destination=vd`, `vectorSource=vs2`
  y un `VectorRegRef` para `vs1` como segundo operando.
- `vadd.vx`: `ArithmeticCommand` de suma, `destination=vd`, `vectorSource=vs2`
  y el valor escalar de `rs1` como segundo operando.
- `vse32.v`: `MemoryCommand` de store, `dataReg=vs3`, base virtual,
  `UnitStrideAddress`, EEW de 32 bits y estado RVV.

Los campos para otros patrones y operaciones se reservan, pero el frontend
debe rechazarlos mientras no exista una ruta funcional completa.

## Tareas especializadas y finalizaciones internas

Estos mensajes se crean después de la admisión. `AraSequencer` copia la
configuración y el payload ya validados; no consulta otra vez el estado RVV
de la CPU. La unidad comprueba las invariantes de su tarea, no decodifica
instrucciones ni reconstruye el comando por su PC.

```cpp
struct ArithmeticTask
{
    UnitTask task;
    VectorConfig config;
    ArithmeticCommand arithmetic;
    ByteRange destinationRange;
};

struct MemoryTask
{
    UnitTask task;
    VectorConfig config;
    MemoryCommand memory;
    ByteRange dataRange;
    Addr pc;
    RequestorID requestorId;
};
```

`ArithmeticTask.task.unit == VectorUnitClass::Lanes` y
`MemoryTask.task.unit == VectorUnitClass::Vlsu`. El contexto está en
`task.key.command.contextId`. `pc` y `requestorId` se conservan desde
`VectorCommand` hasta las peticiones al backend de memoria.

En el baseline, una orden con elementos ejecutables genera una sola tarea
que cubre `[vstart, vl)`. El contrato conserva `TaskKey` para futuras
particiones, pero el reparto aritmético actual se hace mediante fragmentos
de lane subordinados a esa tarea. Con `vstart >= vl` no se crea ninguna tarea,
no se accede al VRF ni a memoria y se emite `Success`, con `finalVstart = 0`.

Para una tarea de elementos de 32 bits:

```text
range.offset = task.elements.firstElement * 4
range.size   = task.elements.elementCount * 4
```

Se calcula sin desbordamiento y se comprueba que el rango cabe en el grupo
efectivo. En aritmética `range` es `destinationRange`; el mismo intervalo se
lee en cada fuente vectorial porque sólo se admite `SameWidth`. En memoria
es `dataRange`, destino de carga o fuente de store. La base del intervalo es
el primer byte del grupo, nunca el primer elemento activo.

### Identidad del fragmento de lane

```cpp
using LaneFragmentId = uint32_t;
inline constexpr LaneFragmentId InvalidLaneFragmentId =
    std::numeric_limits<LaneFragmentId>::max();

struct LaneFragmentKey
{
    TaskKey task;
    LaneFragmentId fragmentId;
};

struct LaneTask
{
    LaneFragmentKey key;
    LaneId laneId;
    ArithmeticCommand arithmetic;
    ElementRange elements;
    ByteRange destinationRange;
    ByteRange vectorSourceRange;
    std::optional<ByteRange> secondVectorSourceRange;
};
```

`TaskDistributor` asigna `fragmentId` sin reutilizarlo dentro de una tarea.
La clave completa incluye el `TaskKey` original; no lo sustituye.
`laneId` es una propiedad del fragmento, no su identidad: una lane puede
recibir varios fragmentos de una misma tarea.

Cada `LaneTask` abarca elementos contiguos, dentro de los de su tarea padre,
y como máximo una palabra del destino. El distribuidor corta también en
cualquier límite de palabra de las fuentes. Todos sus accesos pertenecen a
la lane indicada según `AddressMapper`. Los fragmentos cubren exactamente
los elementos de la tarea sin solapamientos ni huecos; intervalos separados
en una lane se representan mediante varios `LaneTask`, no mediante un
`ElementRange` que incluya elementos de otras lanes.

`arithmetic` se copia de la tarea padre. Los rangos se recalculan para el
fragmento con la fórmula anterior. `secondVectorSourceRange` sólo existe
cuando `secondOperand` contiene `VectorRegRef`; para `vadd.vx` está ausente
y el valor escalar viaja en la alternativa `RegVal` de `secondOperand`.
No hay campos `opcode`, máscara o forma `vv/vx` paralelos que puedan
contradecir ese payload. Las operaciones distintas de Add de 32 bits y sin
máscara siguen rechazándose en admisión.

### Finalización de fragmentos y tareas

```cpp
enum class UnitCompletionStatus : uint8_t
{
    Success,
    MemoryFault,
};

struct LaneCompletion
{
    LaneFragmentKey key;
    UnitCompletionStatus status;
};

struct UnitCompletion
{
    TaskKey key;
    UnitCompletionStatus status;
    std::optional<FaultInfo> fault;
};
```

| Mensaje | Productor → consumidor | Condición terminal |
| --- | --- | --- |
| `LaneCompletion` | `AraLane` → `TaskDistributor` | Todos los accesos y el writeback del fragmento están confirmados. |
| `UnitCompletion` aritmético | `TaskDistributor` → `AraSequencer` | Se han emitido todos los fragmentos y recibido exactamente una finalización de cada uno. |
| `UnitCompletion` de memoria | `AraVLSU` → `AraSequencer` | Todos los elementos terminaron, o la petición fallida quedó cerrada sin trabajo posterior emitido. |

`LaneCompletion.status` sólo puede ser `Success` en el baseline: la suma no
genera faults de memoria y un acceso inválido al VRF es un error interno.
`UnitCompletion` aritmético tampoco lleva fault. Una finalización de memoria
con `MemoryFault` exige `FaultInfo` válido con dirección e índice de elemento;
con `Success` exige su ausencia. El sequencer obtiene `finalVstart` del índice
causante, sin mantener un segundo campo que pueda contradecirlo.

Cada receptor conserva las identidades pendientes, no sólo un contador que
pueda aceptar duplicados. No se completa mientras queden fragmentos por
emitir, accesos aceptados o writebacks sin confirmar. No se añaden errores
internos ni cancelaciones a este protocolo funcional.

## Control de admisión y CommandQueue

**Conexiones directas.** Recibe `requestGrant` y `dispatch` desde
`CpuVectorInterface`, y realimentación de disponibilidad desde
`AraSequencer`. Devuelve `grant`, `accepted` y ocupación a
`CpuVectorInterface`; envía únicamente el comando de cabeza a `AraSequencer`.

### Recibe

- `requestGrant(VectorCommand)` y
  `dispatch(grantToken, VectorCommand)`.
- Espacio liberado en la FIFO.
- Disponibilidad del sequencer y de las unidades del backend.

### Envía

- `grant`, `stall` o `rejected`.
- `accepted` sólo después de almacenar el comando en la FIFO.
- El comando de cabeza hacia `AraSequencer`.
- Señales de `empty`, `full` y ocupación.

### Secuencia de admisión

La validación se reparte sin duplicar la decodificación. `CpuVectorInterface`
comprueba la estructura del descriptor y el extremo VPU comprueba las
capacidades configuradas, las identidades activas y el espacio reservable de
`CommandQueue`. Ambos devuelven sus errores mediante el mismo `GrantResult`;
ninguno interpreta bits de la instrucción original.

`requestGrant` no modifica la FIFO. El control de admisión procesa la consulta
en este orden:

1. `CpuVectorInterface` valida la estructura interna del `VectorCommand`.
2. El extremo VPU comprueba que la configuración y la operación están
   soportadas.
3. Comprueba que el `CommandKey` no tenga otra reserva ni un comando ya
   aceptado.
4. Calcula la capacidad disponible contando tanto entradas ocupadas como
   reservas concedidas todavía no consumidas.
5. Si existe capacidad, crea una reserva, genera un `GrantToken` y devuelve
   `Granted`.

De forma resumida:

```text
descriptor mal formado              -> Rejected(reason)
descriptor válido pero no soportado -> Rejected(reason)
CommandKey ya conocido              -> Rejected(DuplicateCommand)
sin entrada libre reservable        -> Stall
comando válido y entrada reservable -> Granted(token)
```

Un resultado `Rejected` es definitivo para ese descriptor. Un resultado
`Stall` no crea estado ni token; MinorCPU conserva exactamente el mismo
comando y puede repetir la consulta en otro ciclo.

### Validación estructural

La validación estructural comprueba invariantes que no dependen de la
capacidad instantánea de la VPU:

- `CommandKey` tiene un `contextId` y un `commandId` válidos, y
  `requestorId != Request::invldRequestorId`.
- `VectorConfig` contiene un LMUL válido y un SEW de al menos 8 bits que es
  potencia de dos. Un `vstart >= vl` es válido y representa un rango de
  ejecución vacío.
- Todo `VectorRegRef` tiene `regCount != 0`, está contenido en `v0..v31` y
  respeta la alineación del grupo que representa.
- Un `ArithmeticCommand` tiene una operación válida, referencias válidas para
  destino y fuente vectorial, y una alternativa reconocida en
  `ArithmeticOperand`. Si el segundo operando es vectorial, su grupo también
  se valida.
- Un `MemoryCommand` tiene dirección de carga o store, `dataReg` válido, EEW
  no nulo, `fieldCount` entre 1 y 8 y una alternativa reconocida en
  `MemoryAddressing`. `faultOnlyFirst` sólo es estructuralmente válido para una
  carga.
- El tamaño de los grupos representa LMUL en aritmética y EMUL en memoria,
  con un registro contenedor para factores fraccionarios. EMUL se deriva de
  LMUL, EEW y SEW; el grupo resultante debe caber en los 32 registros. La
  sección de capacidad de grupos precisa los límites efectivos en bytes.

Un fallo de estas reglas devuelve `Rejected` con `InvalidIdentity`,
`InvalidVectorConfig`, `InvalidRegisterGroup` o `InvalidPayload`, según
corresponda. Esta comprobación no consulta la ocupación de la FIFO.

### Comprobación del soporte implementado

Un descriptor puede ser estructuralmente correcto según RVV y no estar
implementado por la configuración actual. Para el baseline, la comprobación de
soporte acepta únicamente:

- SEW de 32 bits, el conjunto de LMUL habilitado por la configuración de la
  VPU y `vl` no superior al `VLMAX` que resulta de VLEN, SEW y LMUL.
- Comandos no enmascarados.
- `ArithmeticOperation::Add` con `ElementWidthMode::SameWidth`,
  `ElementSignedness::NotApplicable` y segundo operando `VectorRegRef` o
  `RegVal`.
- Cargas y stores con `UnitStrideAddress`, EEW de 32 bits,
  `fieldCount == 1` y `faultOnlyFirst == false`.

Una operación no implementada devuelve `UnsupportedOperation`; una combinación
de SEW, LMUL, máscara o modo de memoria no implementada devuelve
`UnsupportedConfiguration`. Ninguno de estos rechazos reserva espacio.

### Reserva y consumo del token

La capacidad reservable se calcula como:

```text
available = queueDepth - queuedCommands - outstandingReservations
```

Cuando `available` es cero se devuelve `Stall`. Cuando es mayor que cero, la
VPU crea una entrada de reserva que conserva el `CommandKey` y una copia de
validación del descriptor. Esta copia no equivale a `accepted`: el comando aún
no está en la FIFO y no puede ejecutarse.

`dispatch` busca la reserva mediante `reservationId`, comprueba que el token no
haya sido consumido y compara el `CommandKey` y el descriptor con la copia
validada. Si coinciden, consume la reserva y almacena el comando en la FIFO de
forma atómica; sólo entonces emite `accepted(CommandKey)`. Como la entrada ya
estaba reservada, `dispatch` no puede responder con `Stall`.

Un token inexistente, consumido dos veces o utilizado con otro comando es un
error del protocolo entre CPU y VPU. No produce `Rejected`, porque ese estado
sólo es una respuesta a `requestGrant`; la implementación debe detectarlo con
una aserción o un error fatal de simulación.

Aunque la FIFO pueda almacenar varias órdenes, el sequencer no inicia una
nueva hasta completar la anterior. Si se activa en el futuro el
renombramiento, la admisión comprobará además RAT, free-list y ROB.

### Estado mínimo de admisión y drain

Una reserva contiene `GrantToken` y una copia de `VectorCommand`. El control
de admisión conserva reservas por `reservationId` y las identidades de todos
los comandos reservados o aceptados. `dispatch` consume una reserva y crea
una entrada FIFO, sin cambiar el total de capacidad ocupada. La cabeza activa
sigue contando como entrada de la FIFO hasta su finalización.

`drain` detiene la concesión de nuevas reservas, pero permite consumir los
tokens ya concedidos y terminar comandos aceptados. Minor deja de iniciar
nuevos offloads y de reintentar `requestGrant`. Las consultas válidas que
alcancen la admisión durante drain obtienen `Stall`, sin token. El emisor debe
consumir todo token concedido; el baseline no incorpora cancelación de
reservas.

### Drain conjunto de MinorCPU y VPU

La coordinación es parte del contrato CPU--VPU. No basta con comprobar que la
FIFO está vacía ni con dejar indefinidamente un comando en `WAIT_GRANT`:

| Estado al comenzar drain | Acción y condición de cierre |
| --- | --- |
| Instrucción sin token y sin despacho | Minor puede descartarla por su mecanismo de vaciado, limpiar sus dependencias y preservar el punto arquitectónico para volver a buscarla al reanudar. No hay trabajo que cancelar en la VPU. |
| Reserva concedida | Se conserva la instrucción y se consume el token con `dispatch`, aunque ya no se concedan reservas nuevas. |
| Comando despachado o aceptado | Se protege frente al descarte de instrucciones; se atienden callbacks y se espera a `completed`. |
| Finalización pendiente de procesar | Minor procesa el éxito o fault antes de liberar el estado. Una excepción no se descarta para acelerar drain. |

Una instrucción no enviada que se vuelva a buscar recibe otra identidad al
construir su nuevo comando; no reutiliza la anterior. El punto de reanudación
es el PC arquitectónico que conserva Minor mediante su vaciado normal, no un
PC elegido por la VPU. Las operaciones que sí fueron aceptadas terminan una
sola vez y no se vuelven a ejecutar al reanudar.

La integración futura debe extender los criterios de `Execute::drain`,
`isInbetweenInsts` e `isDrained`, y el descarte de `DrainAllInsts` en
`src/cpu/minor/execute.cc`. Mientras exista reserva, despacho o aceptación
sin cierre CPU, no se puede considerar terminada la instrucción ni eliminar
su estado. Se siguen permitiendo su despacho, finalización y procesamiento
de fault; se detiene el avance de instrucciones posteriores. Es un contrato
por implementar, no una propiedad del código actual de Minor.

La VPU está drenada cuando no quedan reservas, entradas FIFO, tareas,
fragmentos, respuestas ni writebacks pendientes y el backend de memoria
también ha terminado. CPU y VPU sólo están drenadas conjuntamente cuando
también se cumplen los criterios normales de pipeline y LSQ de Minor, no hay
`MinorVectorState` pendiente y no quedan callbacks o eventos con trabajo.
Los eventos de reintento sin trabajo se desprograman. Drain no genera
`Cancelled` ni descarta respuestas.
El reset sólo se admite sin ese trabajo pendiente. Un reset activo y la
cancelación de operaciones de memoria requieren una ampliación posterior.

Estas son estructuras de estado conceptuales; la elección de mapas, conjuntos
o contadores auxiliares corresponde a la implementación. No son un ROB ni
estructuras de disponibilidad de registros.

## AraSequencer

**Conexiones directas.** Recibe comandos de `CommandQueue`, disponibilidad y
finalizaciones de `TaskDistributor` y `AraVLSU`. Envía tareas a esos dos
módulos y comunica la finalización agregada a `CpuVectorInterface`.

### Recibe

- Comando en cabeza de la FIFO con referencias arquitectónicas.
- Referencias de fuentes, que el baseline considera disponibles al iniciar el
  único comando activo.
- Capacidad del distribuidor y de la VLSU; SLDU y MASKU quedan desactivadas.
- Finalizaciones de unidades.

### Envía

- `ArithmeticTask` a `TaskDistributor`, con un `UnitTask` común y el rango de
  bytes de destino propio de la operación aritmética.
- `MemoryTask` a `AraVLSU`, con un `UnitTask` común y un `dataRange` que actúa
  como destino de una carga o fuente de un store.
- En ampliaciones posteriores, `SlideTask`, `PermutationTask`, `MaskTask` y
  tareas de reducción.
- Estado de finalización directamente a `CpuVectorInterface`.

El sequencer asigna el `TaskId` y forma el `TaskKey` antes de enviar una tarea.
La tarea especializada conserva ese sobre común durante todo su recorrido y
la finalización devuelve el mismo `TaskKey`; ningún módulo intermedio genera
una identidad sustitutiva.

En el baseline sólo hay una instrucción activa: la FIFO avanza al terminar la
cabeza. La agregación de todas sus tareas es responsabilidad del sequencer, por
lo que no se necesita un ROB para detectar la finalización del comando.

El estado activo conserva el comando, la identidad de su tarea, si ya fue
aceptada por la unidad y la finalización recibida. Un `Retry` mantiene la
misma tarea pendiente de emisión. La cabeza se libera una sola vez, al cerrar
el comando; el sequencer no deduce finalización de que una cola esté vacía.

## TaskDistributor

**Conexiones directas.** Recibe `ArithmeticTask` de `AraSequencer`, consulta
`AddressMapper`, entrega `LaneTask` a cada lane implicada y recibe
`LaneCompletion`. Devuelve `UnitCompletion` al sequencer. En el baseline sólo
reparte `vadd.vv` y `vadd.vx`.

Acepta una tarea mediante `TransferResult` cuando puede conservar su estado.
El reparto usa exclusivamente el mapeo compartido. Obtiene los fragmentos
contiguos de cada palabra, les asigna `LaneFragmentKey` y envía cada uno a su
lane propietaria. La geometría inicial garantiza que las fuentes y el destino
de una suma de igual anchura sitúan el mismo índice de elemento en la misma
lane, aunque sus bancos o filas sean distintos.

El estado de distribución conserva la tarea padre, el siguiente identificador
libre, los fragmentos por emitir y las claves aceptadas pendientes de
finalización. `Retry` no consume una identidad nueva ni duplica un fragmento.
Al recibir `LaneCompletion`, comprueba y elimina su clave pendiente. Sólo
emite `UnitCompletion` cuando ya no queda trabajo por emitir ni completar.

No envía finalizaciones por lane al sequencer ni genera `TaskKey` nuevos.
El sequencer conoce tareas; el distribuidor conoce sus fragmentos.

## AraLane

**Conexiones directas.** Recibe `LaneTask` de `TaskDistributor` y respuestas
de su `LaneRegisterFile`. Envía accesos al VRF local, `ExecutionBundle` a su
ALU y `LaneCompletion` al distribuidor. En la ruta inicial, la VLSU accede
directamente al LRF propietario: sus cargas no pasan por la ALU ni crean
`LaneTask` aritméticos. Interconexión, MUL, FPU y readiness quedan inactivas.

La lane conserva por fragmento las lecturas por emitir, los accesos aceptados,
los operandos recibidos, la operación en curso y el writeback pendiente. Las
colas de operandos y de writeback pertenecen a la lane, no a los bancos.
Antes de emitir una lectura se reserva capacidad para su respuesta.

Cada acceso al VRF se asocia localmente a la clave del fragmento y a su papel
(fuente vectorial, segunda fuente o destino). Las fuentes se leen antes de
escribir el resultado del fragmento, también cuando coinciden con el destino.
La ALU no provoca la finalización: ésta espera al `WriteAck` del VRF.

### Estado privado de fragmento y accesos pendientes

`LaneFragmentState` pertenece a la lane receptora. Tiene los siguientes
campos conceptuales; no se impone un contenedor concreto para almacenarlos:

| Campo | Contenido e invariante |
| --- | --- |
| `task` | `LaneTask` aceptado; su `key` identifica esta entrada hasta finalizar. |
| `phase` | Lectura de operandos, ejecución, escritura o cierre. No se avanza por el mero envío de una petición. |
| `lhs`, `rhs` | Buffers opcionales de valores de 32 bits. Un buffer presente tiene `elementCount` valores. `rhs` puede proceder del escalar capturado. |
| `pendingReads` | Claves de lecturas por emitir o esperando respuesta, con su papel de fuente. |
| `executionAccepted` | Indica que la ALU aceptó el bundle; desde ese momento no se reenvía. |
| `result` | `ExecutionResult` recibido, retenido hasta preparar y confirmar el writeback. |
| `writeback` | Clave del acceso de escritura, cuando se haya preparado. Su aceptación no equivale a confirmación. |
| `completionSent` | Se activa una sola vez tras `WriteAck` y antes de notificar al distribuidor. |

Los buffers se reservan antes de emitir sus lecturas y antes de aceptar el
bundle en la ALU. Una respuesta puede llegar mientras se esperan otras:
`lhs` y `rhs` se asocian por clave y papel, nunca por orden de llegada.
El resultado debe pertenecer al fragmento en ejecución. Tras la confirmación
de escritura se liberan los buffers y accesos; la entrada se elimina después
de emitir su única `LaneCompletion`.

Cada emisor de accesos al VRF, lane o VLSU, mantiene `PendingVrfAccess`:

| Campo | Contenido e invariante |
| --- | --- |
| `request` | `VrfReadRequest` o `VrfWriteRequest` completo; contiene `VrfAccessKey`, rango, máscara y datos cuando correspondan. |
| `owner` | `LaneFragmentKey` para la lane, o pareja `(TaskKey, requestId)` para la VLSU. El `TaskKey` coincide con el del acceso. |
| `role` | Primera fuente, segunda fuente, destino aritmético, datos para store o writeback de carga. Debe coincidir con la dirección del acceso. |
| `phase` | Preparado para enviar o esperando respuesta. `Retry` conserva la primera fase y todos los campos. |

La entrada y el espacio de respuesta existen antes de invocar al LRF. Se
prepara la recepción antes de la llamada; si ésta devuelve `Retry`, se
restablece el estado de no aceptado. Si un callback síncrono ya resolvió el
acceso, el retorno no lo recrea ni sobrescribe su progreso.

Al recibir `ReadResponse` o `WriteAck`, se comprueba clave, propietario, papel
y tipo de respuesta, se entrega el resultado al estado propietario y se retira
la entrada. Un acceso resuelto no se reenvía. La lane sólo puede cerrar un
fragmento cuando no le quedan entradas pendientes; la VLSU aplica la misma
regla al elemento activo. No se conserva el acceso hasta finalizar todo el
comando si su respuesta ya se consumió.

### Contrato de la ALU del baseline

```cpp
struct ExecutionBundle
{
    LaneFragmentKey key;
    ArithmeticOperation operation;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    std::vector<uint32_t> lhs;
    std::vector<uint32_t> rhs;
};

struct ExecutionResult
{
    LaneFragmentKey key;
    ElementRange elements;
    VectorRegRef destination;
    ByteRange destinationRange;
    std::vector<uint32_t> values;
};
```

La lane produce un bundle por fragmento y la ALU devuelve un resultado. Las
listas tienen exactamente `elements.elementCount` valores y su orden coincide
con los índices crecientes del rango. `operation` sólo admite `Add`. En
`vadd.vv`, `lhs` y `rhs` proceden de las dos lecturas. En `vadd.vx`, la lane
normaliza el escalar a 32 bits y replica su patrón de bits en `rhs`.

La suma conserva los 32 bits bajos, sin overflow de enteros con signo en C++.
No produce excepción aritmética. La lane convierte entre bytes del VRF y
valores de 32 bits según el orden de bytes del objetivo simulado, sin depender
del endianness del host ni reinterpretar buffers mediante casts inseguros.

Clave, elementos y destino se devuelven sin cambios. El receptor verifica
que correspondan a un fragmento pendiente. La aceptación sigue
`TransferResult`; la capacidad del resultado se reserva al admitir el bundle.
La ALU no accede al VRF, a memoria ni al sequencer. No se fijan todavía
latencias o throughput. Las operaciones y excepciones de MUL/FPU se definirán
cuando se amplíe el conjunto de instrucciones.

## LaneRegisterFile, AddressMapper y bancos

### LaneRegisterFile

**Conexiones directas.** Recibe accesos de `AraLane` y `AraVLSU`, consulta
`AddressMapper` y accede exclusivamente a sus bancos locales. Devuelve datos o
confirmación al solicitante original. SLDU, MASKU y readiness no intervienen
en el baseline.

```cpp
enum class VrfRequesterKind : uint8_t
{
    Lane,
    Vlsu,
};

struct VrfRequester
{
    VrfRequesterKind kind;
    std::optional<LaneId> laneId;
};

using VrfAccessId = uint32_t;
inline constexpr VrfAccessId InvalidVrfAccessId =
    std::numeric_limits<VrfAccessId>::max();

struct VrfAccessKey
{
    TaskKey task;
    VrfRequester requester;
    VrfAccessId accessId;
};

struct VrfAccess
{
    VrfAccessKey key;
    VectorRegRef reg;
    ByteRange range;
    ByteEnable byteEnable;
};

struct VrfReadRequest
{
    VrfAccess access;
};

struct VrfWriteRequest
{
    VrfAccess access;
    ByteBuffer data;
};

struct ReadResponse
{
    VrfAccessKey key;
    ByteBuffer data;
};

struct WriteAck
{
    VrfAccessKey key;
};
```

`VrfRequester.laneId` existe sólo para `Lane`; identifica al emisor, no a una
lane destino de la VLSU. Cada solicitante asigna `accessId` sin reutilizarlo
dentro de la tarea. La clave completa es `(TaskKey, requester, accessId)`.
Una lane mantiene la asociación de esa clave con `LaneFragmentKey` y el papel
del operando. La VLSU la asocia con `(TaskKey, requestId)` y la fase del elemento.
No se usa `requestId` de memoria como identificador de un acceso al VRF.

`range` es relativo al principio de `reg`. `byteEnable.size() == range.size`;
los buffers de escritura y lectura tienen también `range.size` bytes. Una
lectura devuelve 0 en posiciones deshabilitadas y el consumidor no las usa
como operandos; una escritura conserva los bytes deshabilitados. El baseline
sólo emite rangos de elementos activos, sin escribir los bytes anteriores a
`vstart` ni los posteriores a `vl`.

Cada acceso emitido al LRF cabe en una palabra y pertenece a ese LRF. Si una
operación lógica abarca varias palabras, el solicitante la divide consultando
el mapper y genera accesos con claves distintas; conserva la asociación para
reunir sus respuestas. Una petición dirigida a una lane equivocada es un
error interno, no un acceso remoto implícito.

El LRF devuelve `Accepted` sólo cuando concede el acceso al banco y está
reservado el espacio para la respuesta. En caso contrario devuelve `Retry`
sin realizar lecturas o escrituras. No encola solicitudes denegadas. Una
lectura aceptada genera un único `ReadResponse`; una escritura aceptada
produce un único `WriteAck` después de aplicar sus bytes habilitados.
No se reenvía una escritura aceptada mientras se espera su confirmación.

### AddressMapper y geometría compartida

Es un servicio de cálculo sin cola ni latencia propia. Lo consultan
`TaskDistributor`, `LaneRegisterFile` y `AraVLSU`. No recibe tareas ni asigna
identidades. La configuración es común a todos ellos y no cambia con trabajo
activo.

```cpp
struct VrfGeometry
{
    uint32_t vlenBytes;
    uint32_t laneWordBytes;
    uint32_t numLanes;
    uint32_t banksPerLane;
};

struct MappedVrfFragment
{
    LaneId laneId;
    BankId bankId;
    uint64_t row;
    uint32_t byteOffsetInWord;
    ByteRange originalRange;
    ByteEnable wordByteEnable;
};

using VrfMapping = std::vector<MappedVrfFragment>;
```

Entrada del mapper: `VectorRegRef`, `ByteRange`, `ByteEnable` del rango y la
geometría. Salida: fragmentos ordenados por desplazamiento original, cada uno
contenido en una palabra. `originalRange` sigue siendo relativo al grupo;
no se sustituye por una dirección local de banco. `wordByteEnable` tiene
`laneWordBytes` entradas y coloca la máscara original en su posición dentro
de la palabra, con ceros fuera del fragmento. La colección cubre todo el rango
original exactamente una vez, incluidos sus bytes deshabilitados.

La geometría inicial exige valores positivos, palabras múltiplo de 4 bytes y
`vlenBytes` múltiplo de `laneWordBytes * numLanes`. Esto alinea el comienzo de
cada registro con el ciclo de reparto entre lanes y evita que un elemento de
32 bits cruce palabras del VRF. El número de bancos no necesita dividir el
número de palabras locales; la última fila puede quedar parcialmente usada.
Los productos y direcciones se calculan con aritmética comprobada de 64 bits.

Para el byte `b` relativo al grupo:

```text
absoluteByte     = reg.firstReg * vlenBytes + b
globalWord       = absoluteByte / laneWordBytes
laneId           = globalWord % numLanes
localWord        = globalWord / numLanes
bankId           = localWord % banksPerLane
row              = localWord / banksPerLane
byteOffsetInWord = absoluteByte % laneWordBytes
```

Cada banco reserva las filas necesarias para los 32 registros arquitectónicos;
se pueden dimensionar con el techo de
`(32 * vlenBytes / laneWordBytes) / (numLanes * banksPerLane)`.
La fórmula incluye `firstReg`: dos registros no pueden aliasar por compartir
el mismo desplazamiento relativo. Las fórmulas se implementarán sólo en
`AddressMapper`; el resto de módulos usará sus resultados.

Por ejemplo, con `vlenBytes=32`, `laneWordBytes=8`, dos lanes y dos bancos,
el rango `[0, 16)` de `v1` se divide en `[0, 8)` en lane 0, banco 0, fila 1,
y `[8, 16)` en lane 1, banco 0, fila 1. Los elementos 0 y 1 pertenecen a
lane 0 y los elementos 2 y 3 a lane 1. Los elementos 4 y 5 vuelven a lane 0,
pero forman otro fragmento, no un rango contiguo con los primeros.

### Capacidad de grupos y configuración admitida

VLEN se obtiene de `vlenBytes * 8` y debe coincidir con el estado RVV de
MinorCPU. La configuración de la VPU declara explícitamente un conjunto no
vacío `supportedLmuls`; no se deduce del enum `VectorLmul`. El soporte de SEW
queda fijado a 32 bits en este hito. Se comprueba
`VLMAX = (VLEN / SEW) * LMUL` con aritmética exacta y `vl <= VLMAX`.

Para LMUL/EMUL entero, `regCount` es 1, 2, 4 u 8, con alineación natural y
sin exceder `v31`. Para un factor fraccionario se usa `regCount=1`, pero sólo
es accesible la fracción efectiva inicial del registro; el factor procede de
la configuración de la tarea y no se codifica mediante un `regCount` cero.
El enum permite describirlo, pero sólo se admite si está en `supportedLmuls`.
El baseline tiene EEW=SEW, por lo que EMUL=LMUL.

El mapper verifica los límites físicos de `VectorRegRef`; el creador de la
tarea o acceso comprueba además la capacidad efectiva según LMUL/EMUL y el
rango activo. Los helpers actuales de `VectorRegRef` y `ByteRange` sólo
comprueban parte de estas invariantes; no son validadores completos de RVV.
Una geometría incompatible es un error de configuración al construir la VPU,
no un `Stall` de ejecución. No se fijan aquí valores concretos del benchmark.

### Banco de VRF

**Conexiones directas.** Recibe accesos ya mapeados exclusivamente desde
`LaneRegisterFile`. Devuelve datos, confirmaciones o `retry` a
`LaneRegisterFile`, que los reenvía al solicitante original.

El LRF arbitra entre solicitantes y entrega accesos ya mapeados. El banco
devuelve aceptación, datos tras su latencia o confirmación de escritura. Si
no puede aceptar, devuelve `Retry` sin efectos. Una política inicial del LRF
puede arbitrar round-robin entre operandos, writeback y VLSU; SLDU/MASKU siguen
fuera del baseline.

```cpp
struct BankReadRequest
{
    VrfAccessKey key;
    uint64_t row;
    ByteEnable wordByteEnable;
};

struct BankWriteRequest
{
    VrfAccessKey key;
    uint64_t row;
    ByteEnable wordByteEnable;
    ByteBuffer wordData;
};

struct BankReadResponse
{
    VrfAccessKey key;
    ByteBuffer wordData;
};

struct BankWriteAck
{
    VrfAccessKey key;
};
```

Lane y banco están determinados por el objeto receptor. `row` debe caber en
su almacenamiento; `wordByteEnable` y `wordData` tienen `laneWordBytes`
entradas. El LRF expande el rango original a posiciones de palabra con ayuda
del mapper. Los bytes exteriores llevan máscara 0; su dato de escritura es
irrelevante y puede inicializarse a 0. El banco conserva los bytes con máscara
0 y devuelve 0 en esas posiciones de una lectura.

Cada acceso del baseline cabe en una palabra, por lo que se conserva
`VrfAccessKey` sin asignar una segunda identidad interna. El LRF retiene el
acceso y su mapeo hasta la respuesta: compacta `BankReadResponse.wordData` al
rango original para construir `ReadResponse`, o convierte `BankWriteAck` en
`WriteAck`. Sólo confirma después de la escritura efectiva del banco.
La capacidad para ese retorno se reserva antes de conceder el acceso.

El banco no resuelve registros arquitectónicos ni decide tareas o destinos
de respuesta globales; siempre responde a su LRF. Una solicitud denegada
permanece en el emisor original, sin crear una entrada de espera en el banco.
La latencia concreta y el número de puertos quedan para el modelo temporal;
estos mensajes sólo fijan datos, propiedad y condición de confirmación.

El banco no incorpora una FIFO de peticiones. El arbitraje del LRF selecciona
los accesos que pueden usar sus puertos; una petición no concedida permanece
pendiente en el solicitante y se reintenta. Las lecturas concedidas alimentan
las colas de operandos, cuya capacidad debe comprobarse antes de emitirlas.
Los resultados esperan en las colas de writeback hasta obtener acceso al
banco. Los registros que temporizan una respuesta no equivalen a una FIFO
de peticiones por banco.

Esta separación sigue la organización del
[VRF de Ara](https://pulp-platform.github.io/ara/modules/lane/vrf.html):
bancos, arbitraje de acceso y colas de operandos/resultados diferenciados.

## VectorInterconnect

**Conexiones directas.** Recibe paquetes de una `AraLane` origen y, cuando se
use distribución de cargas, de `AraVLSU`. Entrega el paquete a la `AraLane`
destino y devuelve `ready/stall` al emisor.

### Recibe

```text
InterconnectPacket(taskKey, sourceLane, destinationLane, kind,
                   registerRef, byteRange, data, last)
```

- `taskKey` identifica la tarea que espera el paquete y conserva la identidad
  completa del comando padre.
- `sourceLane` y `destinationLane` especifican el salto lógico, incluso si la
  implementación posterior lo encamina por varios nodos físicos.
- `kind` clasifica la transferencia (`slide`, `gather`, reducción, etc.) para
  que el receptor interprete `data` y aplique el orden apropiado.
- `registerRef` y `byteRange` indican el grupo arquitectónico y los bytes a los
  que pertenecen los datos; `TaskKey` evita confundir tareas o comandos
  sucesivos.
- `data` contiene el fragmento transferido y `last` marca el último paquete de
  la secuencia necesaria para completar ese rango o esa tarea.

### Envía

- El paquete a la lane destino.
- `ready/stall` al origen.
- Confirmación opcional de entrega.

`kind` distingue al menos `slide`, `gather`, `reduction`,
`load_distribution` y `operand_forward`. Una primera implementación ideal
puede mantener esta interfaz y sustituirse después por ring o crossbar.

La ruta inicial usa el acceso directo de la VLSU al LRF propietario y no
necesita slide, gather ni reducción. `InterconnectPacket` queda como contrato
de ampliación; no condiciona admisión ni finalización del baseline.

## AraVLSU

**Conexiones directas.** Recibe `MemoryTask` de `AraSequencer`, datos y
confirmaciones de `LaneRegisterFile` y respuestas de `VectorMemoryBackend`.
Envía peticiones de memoria, accesos al LRF propietario y `UnitCompletion`
al sequencer. La ruta inicial conecta VLSU y LRF directamente.

Sólo ejecuta `vle32.v` y `vse32.v` unit-stride. Recorre los elementos de la
tarea en orden creciente, con un único elemento en curso. No inicia otro
hasta recibir `WriteAck` de una carga o `StoreAck` de un store. Esta regla
incluye el tiempo de espera por arbitraje o retry y elimina el solapamiento
de peticiones en el primer hito.

### Petición lógica al backend de memoria

```cpp
using RequestId = uint32_t;
inline constexpr RequestId InvalidRequestId =
    std::numeric_limits<RequestId>::max();

struct VectorMemoryRequest
{
    TaskKey taskKey;
    RequestId requestId;
    MemoryDirection direction;
    VectorRegRef registerRef;
    ByteRange dataRange;
    uint32_t elementIndex;
    LaneId laneId;
    Addr virtualAddress;
    uint32_t size;
    ByteBuffer storeData;
    ByteEnable byteEnable;
    Addr pc;
    RequestorID requestorId;
};
```

La VLSU asigna un identificador creciente por tarea al preparar cada elemento;
no se reutiliza dentro de ella, ni cambia al reintentar. La clave de la
petición es **`(TaskKey, requestId)`**, incluido el contexto del comando. No
se introduce aún un tipo `RequestKey`. La VLSU conserva esa pareja hasta
terminar el elemento, también después de recibir datos de carga y mientras
espera su escritura en el VRF.

Invariantes del baseline:

- `direction` es `Load` o `Store`; no hay dos booleanos independientes.
- `size == 4`, `dataRange == ByteRange{elementIndex * 4, 4}` y
  `virtualAddress == memory.base + elementIndex * 4`. El índice es absoluto
  dentro del vector, no relativo a `vstart`.
- `registerRef` se copia de `MemoryTask.memory.dataReg`; es destino en cargas
  y fuente en stores. `dataRange` sirve en ambos casos.
- `laneId` procede del mapper. Un elemento ocupa una palabra de una sola lane
  bajo las restricciones de geometría del baseline.
- `byteEnable` tiene cuatro entradas a 1. `storeData` está vacío en cargas y
  tiene cuatro bytes obtenidos del VRF en stores.
- `pc` y `requestorId` se copian de la tarea. El contexto está en
  `taskKey.command.contextId`; el backend no recupera una instrucción CPU.
- El cálculo de dirección sigue el ancho de dirección del objetivo, sin
  overflow de enteros con signo del host. No se confunde un error de
  traducción de una dirección con un error de identidad del protocolo.

Los metadatos de accesos indexados, segmentados, ordenados o fault-only-first
no se añaden al mensaje activo: sus comandos se rechazan en admisión. El
orden de emisión de este baseline lo determina el recorrido secuencial de
la VLSU, no un campo `orderingMetadata` sin semántica definida.

### Datos de carga y escritura al VRF

```cpp
struct LoadData
{
    TaskKey taskKey;
    RequestId requestId;
    VectorRegRef destinationReg;
    uint32_t elementIndex;
    LaneId laneId;
    ByteRange destinationByteRange;
    ByteBuffer data;
};
```

La VLSU construye `LoadData` al recibir una respuesta correcta usando la
referencia, rango, lane e índice guardados en su petición original. `data`
contiene cuatro bytes. Este descriptor conserva la correlación de memoria
hasta el writeback; no necesita una cola o un módulo adicionales.

En la ruta directa, la propia VLSU transforma `LoadData` en
`VrfWriteRequest`, asigna `VrfAccessKey` y conserva la asociación entre ambas
identidades hasta `WriteAck`. No lo entrega a la ALU ni considera escrita la
carga al recibir solamente `LoadData`. Si en el futuro se transporta mediante
una interconexión, ésta deberá mantener el mismo contrato de confirmación.

Para stores, la VLSU asigna primero `requestId` y un acceso de lectura al VRF.
Tras `ReadResponse` incorpora los bytes a `storeData` y puede enviar la
petición al backend. Un retry del backend no obliga a releer el registro ni
modifica el buffer ya capturado.

### Estado del elemento y terminación

El estado mínimo guarda `MemoryTask`, siguiente índice, siguiente
`RequestId` y una entrada opcional del elemento activo. Esta entrada conserva
la petición lógica, fase, clave del acceso al VRF y, cuando corresponde, los
datos de respuesta o el fault. Una tabla futura de varias entradas se
indexaría por la pareja completa `(TaskKey, requestId)`.

```text
Carga:  PREPARE -> SEND_MEMORY -> WAIT_MEMORY
        -> SEND_WRITEBACK -> WAIT_WRITE_ACK -> NEXT_ELEMENT
Store:  PREPARE -> SEND_VRF_READ -> WAIT_VRF_READ
        -> SEND_MEMORY -> WAIT_MEMORY -> NEXT_ELEMENT
Fault:  WAIT_MEMORY -> TASK_MEMORY_FAULT
```

Los estados `SEND_*` conservan mensaje y clave ante `Retry`; sólo avanzan a
`WAIT_*` tras `Accepted`. `NEXT_ELEMENT` libera el estado del elemento y
avanza, o emite `UnitCompletion(Success)` si terminó el último.

En `TASK_MEMORY_FAULT`, la respuesta ya es terminal para el backend: no quedan
subpeticiones físicas pendientes. No se emiten nuevos elementos ni writeback
para una carga fallida. Los elementos anteriores permanecen completados.
La VLSU devuelve `UnitCompletion(MemoryFault)` con el fault, la dirección
causante y `elementIndex`. El sequencer genera `VectorCompletion` con ese
índice como `finalVstart`. No necesita cancelar otras peticiones lógicas,
porque sólo había una en curso.

## VectorMemoryBackend

Es la capa que adapta peticiones lógicas a `Request`, `Packet`, traducción y
puertos de gem5. Recibe `VectorMemoryRequest` de la VLSU y devuelve una sola
respuesta terminal por petición aceptada.

### Respuestas normalizadas

```cpp
enum class MemoryResponseStatus : uint8_t
{
    LoadData,
    StoreAck,
    Fault,
};

struct VectorMemoryResponse
{
    TaskKey taskKey;
    RequestId requestId;
    MemoryResponseStatus status;
    ByteBuffer data;
    std::optional<FaultInfo> fault;
};
```

| Estado | Petición original | `data` | `fault` |
| --- | --- | --- | --- |
| `LoadData` | `Load` | Exactamente `size` bytes, en orden de dirección | Ausente |
| `StoreAck` | `Store` | Vacío | Ausente |
| `Fault` | `Load` o `Store` | Vacío | Objeto `Fault` válido, dirección virtual causante e índice del elemento original |

La VLSU valida identidad, tamaño y compatibilidad del estado con la dirección
de la petición original antes de consumir una respuesta. El backend conserva
`TaskKey`, `requestId`, `elementIndex`, `pc`
y `requestorId` durante traducción y envío. Obtiene el contexto de traducción
a partir de `contextId`, sin filtrar `ThreadContext`, `Packet` o punteros de
instrucción hacia las lanes. El soporte inicial sigue siendo SE.

### Fragmentación y retry de gem5

Una petición lógica contiene un elemento completo aunque el acceso físico
necesite dividirse por límites de línea, página o por requisitos del puerto.
El backend conserva internamente cada fragmento, su desplazamiento en el
buffer lógico y su estado de traducción/envío/respuesta. Esos fragmentos no
reciben nuevos `requestId` visibles en la VLSU.

El backend acepta la petición mediante `TransferResult` sólo si puede guardar
su descriptor, datos y respuesta terminal. Una vez aceptada, los retries del
puerto (`recvReqRetry`) son responsabilidad del backend: la VLSU no vuelve a
enviar la petición lógica. Un paquete que el puerto no ha aceptado conserva
sus datos y estado hasta el reintento. Nunca se reenvía un paquete aceptado
por no haber recibido todavía su respuesta.

Para el baseline, los fragmentos físicos se sirven sin solapamiento. Antes
de enviar un store fragmentado, se completan las traducciones necesarias;
un fallo de traducción no debe emitir sólo una parte de ese store. En una
carga, los bytes se reúnen antes de responder y no se publica un resultado
parcial en el VRF. `StoreAck` exige confirmar todos los fragmentos del store.

Un fault conserva la dirección virtual del fragmento causante y el índice
del elemento lógico. Se detiene la emisión de fragmentos posteriores y se
cierra cualquier estado pendiente antes de entregar `Fault`. Esto no añade
rollback ni garantiza atomicidad de un store que ya haya producido efectos
antes de un error de acceso; no se anuncia como éxito ni se reenvía de forma
automática. La política de alineación y los faults del objetivo se respetan
en la adaptación a gem5, sin inventar una alineación distinta en las lanes.

Durante drain se sigue atendiendo traducción, retry y respuestas del trabajo
aceptado hasta vaciar ese estado. No existe `MemoryResponseStatus::Cancelled`
en el baseline. La cancelación por reset requiere un contrato posterior.

### Estado privado y propiedad de objetos de gem5

`MemoryTransactionState` es propiedad de `VectorMemoryBackend`; existe una
entrada por petición lógica aceptada, como máximo una en el baseline:

| Campo | Contenido e invariante |
| --- | --- |
| `request` | Copia de `VectorMemoryRequest`; conserva la clave lógica y los metadatos CPU. |
| `data` | Buffer de `request.size` bytes para reunir la carga o conservar el store. No se publica parcialmente. |
| `fragments` | Entradas `MemoryFragmentState` que cubren el acceso sin huecos ni solapamientos. |
| `nextFragment` | Fragmento que puede enviarse a memoria una vez traducido. No hay dos fragmentos físicos enviados sin respuesta a la vez. |
| `pendingTranslations` | Traducciones iniciadas cuyos callbacks todavía no terminaron. Impide liberar la transacción antes de tiempo. |
| `fault` | Primer fallo terminal registrado, asociado a su dirección virtual y elemento lógico. Impide emitir fragmentos posteriores. |
| `responseSent` | Marca de respuesta lógica emitida una sola vez, después de cerrar todo el trabajo físico. |

Cada `MemoryFragmentState` contiene:

| Campo | Contenido e invariante |
| --- | --- |
| `offset`, `size` | Intervalo no vacío dentro del buffer lógico; la suma no desborda ni excede el tamaño de la petición lógica. |
| `virtualAddress` | Dirección del fragmento derivada de la petición original con el ancho de dirección del objetivo. |
| `physicalAddress` | Opcional; sólo existe después de una traducción correcta. |
| `phase` | Pendiente de traducción, traduciendo, listo para enviar, esperando retry del puerto, esperando respuesta, terminado o fallido. |
| `request` | Referencia gestionada al `Request` de gem5 mientras sea necesaria para traducción o paquete. |
| `retryPacket` | Paquete todavía no aceptado por el puerto, cuando se está preparando el envío o esperando retry. |
| `fault` | Causa local del fallo; permite construir el `FaultInfo` de la transacción. |

Los callbacks de traducción y el estado de retorno del paquete identifican la
transacción y el índice de fragmento. Son referencias privadas del backend;
no crean nuevos `requestId` ni cruzan hacia las lanes. La transacción y sus
entradas mantienen direcciones estables mientras existan callbacks o paquetes
que las referencien. Un `Packet::SenderState` propio puede transportar esa
correlación; no debe ser un puntero a una instrucción CPU.

| Momento | Propiedad y liberación |
| --- | --- |
| Preparación/traducción | El backend conserva la transacción, el buffer y las referencias `RequestPtr`. No libera el estado de una traducción pendiente. |
| Paquete construido, aún no enviado | El backend posee el paquete y el estado de retorno que ha creado; prepara ambos antes de llamar al puerto. |
| `sendTimingReq` devuelve falso | El backend conserva el mismo paquete en espera. Sólo puede reenviarlo después de `recvReqRetry`; si vuelve a fallar, espera otro aviso. |
| `sendTimingReq` devuelve verdadero | El paquete está en el camino de memoria. El backend no lo modifica, reenvía ni destruye; conserva el estado y buffers que todavía puedan estar referenciados. |
| `recvTimingResp` acepta la respuesta | El backend recupera la correlación, copia los datos o registra el fallo, retira su estado de retorno y libera el paquete recibido una sola vez. |
| Cierre lógico | Sin callbacks de traducción ni paquetes pendientes, se construye la respuesta lógica y se entrega a la VLSU. Después se libera la transacción y sus referencias gestionadas. |

`RequestPtr` se libera soltando la referencia gestionada, no mediante un
`delete` independiente. Los buffers propios de paquete y de transacción no
deben liberar dos veces la misma memoria: la adaptación debe usar copias o
expresar un único propietario. Si un paquete referencia bytes del buffer
lógico, éste vive hasta completar el acceso físico. La respuesta lógica
entregada a la VLSU conserva su propia copia conforme al contrato de mensajes.

Al enviar se reserva recepción suficiente para el único fragmento físico en
curso. El baseline acepta su respuesta sin introducir un segundo protocolo
de backpressure de respuestas. En ningún caso se descarta una respuesta por
falta de espacio. Los estados se preparan antes de llamadas que puedan
invocar callbacks; el retorno no sobrescribe una transición ya completada.

Ante un fallo de traducción, no se envía el paquete correspondiente y se
espera a cerrar los callbacks ya iniciados. Los paquetes preparados pero no
enviados pueden liberarse al cerrar el fallo; los aceptados por memoria nunca
se destruyen mientras estén en vuelo. La respuesta lógica `Fault` sólo sale
cuando se cumplen estas condiciones. Drain usa el mismo cierre, sin cancelar
traducciones ni paquetes aceptados.

## Validación por tipo y estado de implementación

Un helper local no sustituye la validación con contexto. Los productores
construyen mensajes válidos; los receptores comprueban las invariantes de su
frontera y que las respuestas correspondan a trabajo pendiente.

| Tipo o familia | Comprobación local | Comprobación con contexto y responsable | Incumplimiento |
| --- | --- | --- | --- |
| `VpuParameters`, `VrfGeometry` | Capacidades positivas, LMUL válidos y sin duplicados | `VectorEngine`: geometría, tamaños de buffers y coincidencia con VLEN de CPU | Error de configuración antes de ejecutar. |
| `DecodedVectorOp`, `ScalarOperandRef` | Clase y payload coherentes; registros vectoriales en rango | Decode/Execute: índice dentro de la tabla de fuentes, clase escalar y dependencias listas | Error interno si el descriptor CPU está mal formado; una instrucción no soportada sigue el camino de rechazo definido. |
| `VectorConfig` | LMUL y SEW representables; rango vacío permitido | Interfaz valida estructura; admisión comprueba `supportedLmuls`, SEW=32 y `VLMAX` | `InvalidVectorConfig` o `UnsupportedConfiguration`, sin reserva. |
| `VectorRegRef`, `ByteRange` | Límites de registros, rango no vacío y sin overflow | Constructor del comando/interfaz: alineación y capacidad LMUL/EMUL; productor de tarea/acceso: rango efectivo | `InvalidRegisterGroup` en admisión; error interno si llega a una unidad. |
| `VectorCommand` | Identidad, payload y metadatos válidos | Interfaz valida estructura; frontend valida soporte, duplicados y capacidad | `Rejected(reason)` para descriptor inválido/no soportado; `Stall` sólo para indisponibilidad temporal. |
| `GrantToken`, `GrantResult` | Combinaciones de estado, token y motivo coherentes | Admisión: reserva existente, descriptor idéntico y consumo único | Error interno al consumir un token inválido o repetido. |
| `UnitTask`, tareas especializadas | Clave, unidad, elementos y payload coherentes | Sequencer/productor y unidad receptora: pertenencia al comando, rango `[vstart, vl)` y equivalencia con rangos de bytes | Error interno; no es una nueva admisión. |
| `LaneTask`, mensajes de ALU | Clave, tamaños de operandos y rango coherentes | Distribuidor/lane: propiedad según mapper, cobertura y asociación con tarea/fragmento pendiente | Error interno. |
| Accesos VRF y banco | Máscaras, tamaños, filas y claves válidos | Emisor/LRF/banco: capacidad efectiva, mapeo, propietario y respuesta del acceso aceptado | Error interno; falta de capacidad produce `Retry` antes de efectos. |
| Peticiones y respuestas de memoria | Clave, dirección, tamaño y buffers coherentes | VLSU/backend: asociación lógica/física, fragmentos y traducción | Descriptor interno inválido: error interno; fallo de traducción/acceso: respuesta `Fault`. |
| `LaneCompletion`, `UnitCompletion` | Estado y fault compatibles | Distribuidor/sequencer: identidad pendiente y todas las escrituras o peticiones cerradas | Error interno ante duplicados o cierre prematuro. |
| `VectorCompletion`, `FaultInfo` | Combinaciones de la tabla CPU--VPU; dirección e índice obligatorios en `MemoryFault` del baseline | Sequencer/interfaz/Minor: comando aceptado, elemento dentro del rango y `finalVstart` igual al índice causante | Error interno del protocolo; un fault válido se entrega al programa por Minor. |
| Estados privados | Campos válidos para su fase | Módulo propietario: transiciones, propiedad de objetos y ausencia de trabajo antes de liberar | Error interno; no se convierte en excepción arquitectónica. |

Los tipos nuevos de configuración, Decode, estado privado, bancos y memoria
son especificaciones documentales pendientes de implementación. Los tipos
básicos existentes tienen algunos helpers locales; no incluyen todavía todos
los validadores de esta matriz ni el comportamiento de los módulos.

Hay un desajuste pendiente de corregir al implementar
`interface/vector_completion.hh`: su comentario presenta dirección e índice
del fault de memoria como opcionales, mientras el contrato del baseline los
exige. Los campos pueden seguir siendo `std::optional` para otros usos, pero
el validador debe exigirlos en `MemoryFault`. También debe excluir los estados
reservados de la finalización normal del baseline. Esta revisión registra el
desajuste y no modifica la cabecera.

## SLDU y MASKU

Estos módulos no son necesarios para la secuencia funcional inicial. Pueden
existir como estructuras desactivadas, sin reservar recursos ni intervenir en
la admisión o finalización de comandos.

**Conexiones directas futuras.** Ambos recibirán tareas de `AraSequencer` y
datos del VRF; SLDU también recibirá y enviará paquetes por
`VectorInterconnect`. Enviarán lecturas/escrituras a `LaneRegisterFile` y
`UnitCompletion` a `AraSequencer`; MASKU podrá enviar predicados a las lanes.

### SLDU

Recibe tareas de slide, gather o permutación, datos locales y remotos, y
readiness de fuentes. Envía lecturas al VRF, paquetes a la interconexión,
writebacks, actualizaciones de readiness y `UnitCompletion`. Calcula
explícitamente lane origen y destino para cada elemento.

### MASKU

Recibe operaciones de máscara, rangos de bits de fuentes, resultados booleanos,
`vl`, `vstart` y políticas tail/mask agnostic. Envía lecturas/escrituras de
máscara, bits de predicación para lanes, `maskReady`, resultados escalares y
`UnitCompletion`.

Las referencias de máscara de esa ampliación deben usar `maskReg`, `firstBit`
y `bitCount`, en lugar de rangos de bytes. El renombramiento puede
sustituir `maskReg` por `maskVersion`.

## ReadinessTable fuera del mínimo funcional

Con una sola instrucción activa y `chaining_mode = off`, el sequencer puede
esperar a que terminen todas las tareas sin una tabla de disponibilidad
granular. Por tanto, esta tabla no es obligatoria para el primer benchmark;
puede sustituirse por contadores de tareas y peticiones pendientes. La interfaz
siguiente se conserva para introducir chaining después.

**Conexiones directas futuras.** Recibe altas y bajas de comando de
`AraSequencer`, y actualizaciones de bytes o bits de las unidades que escriben
en el VRF (`AraLane`, `AraVLSU`, SLDU o MASKU). Envía consultas y wakeups a los
consumidores que se bloquean: lanes, VLSU, SLDU y MASKU.

### Recibe

- `beginCommand(commandId, destinationReg, ranges)` al iniciar el comando.
- `markBytesReady(commandId, registerRef, range)` tras un writeback.
- `markMaskBitsReady(commandId, maskReg, range)`.
- `endCommand(commandId)` cuando el sequencer agrega todas las finalizaciones.
- Consultas y suscripciones de tareas bloqueadas.

### Envía

- Resultado de `bytesReady` o `maskBitsReady`.
- Wakeups para lanes, VLSU, SLDU y MASKU.
- Diagnósticos de acceso a una referencia o rango inválidos.

```cpp
bool bytesReady(CommandKey command, VectorRegRef reg, ByteRange range);
bool maskBitsReady(CommandKey command, VectorRegRef reg, BitRange range);
void markBytesReady(CommandKey command, VectorRegRef reg, ByteRange range);
void markMaskBitsReady(CommandKey command, VectorRegRef reg, BitRange range);
void endCommand(CommandKey command);
```

Con `chaining_mode = off`, la tabla puede marcar el grupo entero al acabar una
instrucción. La interfaz no cambia al activar posteriormente chaining granular.
La extensión con renombramiento sustituirá `VectorRegRef` por
`PhysicalRegRef` y añadirá la reserva y liberación de versiones.

## Secuencia de vida de un comando

```text
Decode clasifica la macro RVV
  -> vsetvli: MinorCPU actualiza vl/vtype y el destino escalar
  -> vle32.v/vadd.vv/vadd.vx/vse32.v: Execute valida dependencias
     y lee bases u operandos escalares
  -> CpuVectorInterface asigna CommandKey
  -> Minor construye VectorCommand; CpuVectorInterface lo valida
  -> Commit obtiene grant y hace dispatch
  -> admisión almacena el comando y emite accepted
  -> AraSequencer asigna TaskKey y genera ArithmeticTask o MemoryTask
  -> distribuidor genera fragmentos de lane, o VLSU recorre elementos
  -> lanes o VLSU ejecutan y esperan writeback / confirmaciones
  -> distribuidor agrega LaneCompletion; VLSU cierra su petición
  -> AraSequencer recibe UnitCompletion y cierra el comando
  -> CpuVectorInterface emite completed
  -> Minor finaliza la instrucción o entrega el fault
```

La posible optimización futura de retirar la instrucción de Minor en
`accepted`, en vez de hacerlo en `completed`, debe posponerse hasta disponer de
un mecanismo explícito de excepciones precisas y orden de memoria entre la CPU
escalar y la VPU.

### Recorridos de comprobación documental

Estos recorridos comprueban el contrato; no son pruebas implementadas ni
resultados de simulación.

| Caso | Recorrido y condición que debe poder verificarse |
| --- | --- |
| `vadd.vx` | Minor captura `RegVal` → `ArithmeticTask` → fragmentos `LaneTask` con el mismo escalar → lectura de fuente → `ExecutionBundle` → `ExecutionResult` → escritura y `WriteAck` → `LaneCompletion` → `UnitCompletion` → `VectorCompletion`. |
| `vle32.v` | `MemoryTask` → petición del elemento `i` con clave completa → `LoadData` de memoria → descriptor `LoadData` de writeback → acceso de escritura al LRF → `WriteAck` → siguiente elemento. La última escritura precede a `UnitCompletion`. |
| `vse32.v` | Se reserva identidad del elemento `i` → lectura del LRF → datos de store → petición de memoria → `StoreAck` → siguiente elemento. El último ack precede a `UnitCompletion`. |
| Rango vacío | Tras admisión, `vstart >= vl` produce `Success` y `finalVstart=0`, sin construir rangos o tareas vacíos ni acceder a VRF/memoria. |
| Fault en elemento `i` | Los elementos anteriores ya terminaron; la respuesta `Fault` cierra el elemento actual, no se inicia `i+1` y el sequencer devuelve `MemoryFault` con `finalVstart=i`. No hay writeback de una carga fallida. |
| Retry | No cambia la identidad ni transfiere propiedad. Tras aceptación, se espera respuesta; no se repite una operación aceptada. |
| Drain | No se conceden reservas nuevas; se consumen las ya concedidas y terminan todos los accesos aceptados, sin `Cancelled`. |

Los siguientes recorridos cierran la integración y los estados privados:

| Caso | Recorrido y condición que debe poder verificarse |
| --- | --- |
| Dos fuentes y writeback | Dos `PendingVrfAccess` con papeles distintos pueden responder en cualquier orden; la ALU espera ambos operandos. El acceso de destino tiene su propia clave y sólo su `WriteAck` permite finalizar. |
| Retry interno y aceptación | Un solo evento del emisor reintenta el mismo mensaje en el ciclo siguiente. Tras aceptación no hay reenvío; la respuesta reactiva el trabajo habilitado. |
| Retry del puerto | Un envío fallido conserva el paquete y espera `recvReqRetry`. No se activa el sondeo interno para ese paquete ni se duplica la petición de la VLSU. |
| Traducción y fragmentación | Los fragmentos cubren el buffer lógico una sola vez. Sus callbacks mantienen estado válido; un fault impide emitir nuevos fragmentos y espera al cierre de los ya iniciados antes de responder. |
| Drain antes del grant | La instrucción no enviada se descarta mediante Minor y conserva su punto de reanudación. No queda un `WAIT_GRANT` que impida vaciar el pipeline ni se reutiliza su identidad. |
| Drain después del grant | El token se consume aunque la admisión ya esté drenando; la instrucción queda protegida hasta completar. No se abandona una reserva. |
| Drain después de accepted | Se siguen atendiendo respuestas y writebacks, Minor procesa la finalización o fault y sólo después libera el estado. El criterio conjunto incluye LSQ, pipeline y VPU. |
| Callback inmediato | La asociación y recepción existen antes de la llamada. El retorno no sobrescribe la fase avanzada por el callback, no recrea accesos resueltos ni usa estado liberado. |

### Decisiones y alternativas descartadas en este hito

- Un fragmento contiguo por `LaneTask`, frente a extender `ElementRange` para
  representar intervalos dispersos. Se conserva la semántica del tipo actual.
- Identidad de fragmento subordinada a `TaskKey`, frente a identificarlo sólo
  con `laneId`. Una misma lane puede recibir varios fragmentos.
- Una petición lógica de memoria de un elemento en curso, frente a cargas
  solapadas. Reduce el estado de progreso y de fallo sin eliminar identidades.
- `MemoryDirection`, frente a booleanos de carga/store que pueden contradecirse.
- Acceso directo VLSU--LRF, frente a añadir transporte de cargas por las lanes.
- Estado de tareas y accesos pendientes, frente a activar ROB, renombramiento
  o readiness. No se prescribe aún una microarquitectura temporal avanzada.
- Configuración global inmutable, frente a copias modificables por módulo.
  Los valores concretos de ejecución no se deducen de los enums de RVV.
- Semántica conservada en CPU desde Decode, frente a reinterpretar opcodes
  en Commit o entregar objetos de instrucción a la VPU.
- Evaluación funcional tras retry interno y aviso del puerto para retry de
  memoria, sin depender de actividad escalar ajena ni sondear el puerto.
- Descartar mediante Minor sólo instrucciones sin reserva ni despacho al
  drenar; las reservadas o aceptadas terminan y conservan su asociación CPU.

## Extensión futura: renombramiento y ROB

Esta sección documenta una posible evolución y no constituye un requisito de
la primera implementación. En el baseline no se crean la RAT, la free-list ni
el ROB; tampoco se envían `RenameRequest` o `RenameResult`. Si sus clases o
puertos se declaran como armazón inicial, deben permanecer desactivados y sin
participar en admisión, ejecución o finalización. Los registros arquitectónicos
actúan directamente como referencias efectivas y el sequencer comunica la
finalización a `CpuVectorInterface`.

### Renombramiento propuesto

**Conexiones directas futuras.** Recibe `RenameRequest` de `CommandQueue`
durante la admisión y liberaciones de versiones del ROB. Devuelve
`RenameResult` a `CommandQueue` o `AraSequencer`, que lo adjunta al comando
antes de emitirlo; también actualiza RAT, free-list y `ReadinessTable`.

#### Recibe

```text
RenameRequest(commandId, fuentes lógicas, destino lógico,
              grupos LMUL/EMUL, uso implícito de v0)
```

Sus campos son:

- `commandId`: identidad estable del comando; en una implementación con varios
  contextos equivale al `CommandKey` completo (`commandId`, `contextId`).
- `fuentes lógicas`: registros vectoriales arquitectónicos que se leen, antes
  de consultar la RAT. Incluyen todos los registros de un grupo LMUL/EMUL.
- `destino lógico`: registro vectorial arquitectónico que se sobrescribe, o la
  ausencia de destino para una operación que sólo produce un resultado escalar.
- `grupos LMUL/EMUL`: tamaño y alineación de los grupos de fuente y destino
  que la RAT debe resolver y que la free-list debe poder reservar de una vez.
- `uso implícito de v0`: indica que la instrucción está enmascarada y, por
  tanto, `v0` debe tratarse como una fuente aunque no aparezca en el opcode.

También recibe liberaciones de versiones procedentes del ROB.

#### Envía

```text
RenameResult(commandId, sourceVersions[], newDestinationVersion,
             oldDestinationVersion, maskVersion)
```

- `commandId` relaciona el resultado con la petición que lo originó.
- `sourceVersions[]` es la versión física de cada fuente lógica, en el mismo
  orden que en la petición y con todos los registros de cada grupo.
- `newDestinationVersion` es el grupo físico recién reservado para el destino;
  se marca inicialmente como no listo en `ReadinessTable`.
- `oldDestinationVersion` es el mapeo previo del destino lógico. El ROB lo
  conserva para liberarlo al retirar el comando, nunca al renombrarlo.
- `maskVersion` es la versión física de `v0` cuando se usa predicación; queda
  vacía cuando la instrucción no está enmascarada.

Además, invalida el destino recién asignado en `ReadinessTable` y crea una
entrada de versión en el ROB. La asignación de un grupo LMUL ha de ser atómica:
se asigna el grupo completo o se rechaza el comando.

### ReorderBuffer vectorial propuesto

**Conexiones directas futuras.** Recibe entradas asignadas por el bloque de
renombramiento y finalizaciones de `AraSequencer`. Devuelve capacidad a
`CommandQueue`, liberaciones de versiones al renombrador y
`VectorCompletion` ordenado a `CpuVectorInterface`.

#### Recibe

- Una entrada nueva: `commandId`, destino nuevo y destino antiguo.
- Finalización de las tareas de una unidad.
- Datos de excepción y posible resultado escalar.

#### Envía

- `canAllocate` hacia admisión.
- Liberación en orden de la versión física antigua a la free-list.
- Liberación de entradas obsoletas de `ReadinessTable`.
- `VectorCompletion` ordenado hacia `CpuVectorInterface`.

El ROB conserva versiones y retiro en orden; no decide qué instrucción emite
el sequencer.
