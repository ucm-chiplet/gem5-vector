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
  arquitectónico y un rango explícito de bytes, o de bits para máscaras.
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
   |  BackendCommand / UnitTask
   v
Lanes y VLSU <--> VRF distribuido <--> enlace ideal
   |
   |  VectorMemoryRequest / VectorMemoryResponse
   v
Memoria y traducción de gem5

Extensiones: SLDU, MASKU, interconexión temporizada, renombramiento y ROB
```

## Tipos compartidos

Los siguientes tipos son conceptuales; su forma final puede adaptarse a las
convenciones C++ de gem5.

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
  arquitectónicos: `firstReg` es su inicio y `regCount` su tamaño LMUL/EMUL.
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
  opcode ni rangos de registros. `ArithmeticTask` añadirá su
  `destinationRange`, mientras que `MemoryTask` añadirá un `dataRange` que es
  destino para una carga y fuente para un store. Así `UnitTask` no atribuye
  una semántica de destino incorrecta a las tareas de store.

- `VectorCompletion` vive en la frontera de salida VPU--CPU. `AraSequencer` la
  crea una vez agregadas todas las tareas y la envía por `CpuVectorInterface` a
  `MinorCPU::Execute/Commit`. Existe para cerrar el comando, liberar el estado
  en vuelo y transportar resultado escalar, `vstart` o excepción. El resultado
  escalar sólo existe cuando `scalarResult` contiene un valor y `fault` sólo
  debe existir en estados de fallo. `Success` no contiene `fault`; un estado de
  fallo contiene un objeto `Fault` válido. En un `MemoryFault`, `address` y
  `elementIndex` identifican, cuando estén disponibles, el acceso causante.

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
respuesta y la VLSU lo usa para asociarla aunque las respuestas lleguen en otro
orden. Una ampliación posterior podrá formalizar `RequestKey` como la pareja
`(TaskKey, requestId)`. Hasta entonces, las firmas abreviadas deben transportar
el `TaskKey` completo, incluido el `CommandKey` con su `contextId`.

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
2. `WAIT_GRANT`: las dependencias escalares están listas. Execute extrae la
   semántica y los operandos, obtiene un `CommandKey` de
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
   resultado escalar o `vstart`, entrega el fault si lo hay, libera sus
   dependencias y retira la instrucción de `inFlightInsts`.

En `vsetvli` no se recorren estos estados de offload: Execute actualiza
`vl`/`vtype`, escribe el `vl` escalar y completa la instrucción localmente.
Para `vle32.v`, `vadd.vv`, `vadd.vx` y `vse32.v`, mantener la entrada hasta
`COMPLETED` garantiza que los datos de la VPU y los faults lleguen antes de que
la CPU continúe con instrucciones dependientes.

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
- Eventos de reset o drain.

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
- El tamaño de los grupos coincide con LMUL en aritmética y con EMUL en
  memoria. EMUL se deriva de LMUL, EEW y SEW; el grupo resultante debe caber en
  los 32 registros arquitectónicos.

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

## AraSequencer

**Conexiones directas.** Recibe comandos de `CommandQueue`, disponibilidad y
finalizaciones de `TaskDistributor` y `AraVLSU`. Envía tareas a esos dos
módulos y comunica la finalización agregada a `CpuVectorInterface`.

### Recibe

- Comando en cabeza de la FIFO con referencias arquitectónicas.
- Referencias de fuentes, que el baseline considera disponibles al iniciar el
  único comando activo.
- Capacidad de lanes, VLSU, SLDU y MASKU.
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

## TaskDistributor

Para el primer hito sólo distribuye `vadd.vv` y `vadd.vx`. La distribución de
reducciones y otras operaciones aritméticas se incorpora después de validar la
secuencia de referencia.

**Conexiones directas.** Recibe `ArithmeticTask` y los datos de configuración
desde `AraSequencer`, además de capacidad y finalizaciones desde `AraLane`.
Envía un `LaneTask` a cada lane implicada y devuelve la finalización agregada a
`AraSequencer`. Consulta `AddressMapper` para determinar la lane propietaria
de cada fragmento.

### Recibe

- Operación aritmética de suma vectorial.
- `vl`, `vstart`, SEW/EEW, LMUL/EMUL y máscara.
- Referencias arquitectónicas de fuentes y destino.
- Capacidad de cada lane.

### Envía

```text
LaneTask(taskKey, laneId, opcode, sourceRegs, destinationReg,
         elements, byteRanges, mask)
```

- `taskKey` contiene el `CommandKey` padre y el `TaskId` del fragmento;
  permite asociar el writeback y la finalización a la tarea correcta sin
  perder el contexto de CPU.
- `laneId` es la lane que ejecuta el fragmento. `TaskDistributor` la obtiene
  consultando `AddressMapper` con la referencia de registro y el rango
  correspondiente, de modo que el reparto respete la propiedad local del VRF.
- `opcode` selecciona la operación de la unidad, y `sourceRegs` contiene los
  grupos arquitectónicos de todos sus operandos.
- `destinationReg` es el grupo arquitectónico en el que se escribirán los
  resultados de la tarea.
- `elements` es el `ElementRange` asignado a esa lane. No incluye elementos
  anteriores a `vstart` ni fuera de `vl`.
- `byteRanges` traduce esos elementos a los intervalos que se leen o escriben
  en los grupos de registros; puede contener varios rangos para operandos o
  grupos que cruzan registros.
- `mask` identifica el registro arquitectónico y el rango de bits de
  predicación, junto con la política necesaria para distinguir elementos
  inactivos de tail agnostic.

Envía asimismo confirmación de aceptación, finalización por lane y una
finalización agregada al sequencer. Su responsabilidad es repartir las tareas
y agregar sus finalizaciones. Utiliza el mapeo de `AddressMapper`, incluida
la conversión `global_word -> lane_id + local_word`, sin duplicar sus
fórmulas.

## AraLane

**Conexiones directas.** Recibe `LaneTask` de `TaskDistributor`, datos de
`LaneRegisterFile` y cargas de `AraVLSU`. Envía lecturas/escrituras al
`LaneRegisterFile` y finalizaciones a `TaskDistributor` o `AraSequencer`; la
interconexión y `ReadinessTable` son conexiones opcionales de ampliaciones.

### Recibe

- `LaneTask`.
- Respuestas de lectura del VRF.
- Datos de carga de la VLSU.
- Datos remotos de la interconexión.
- Grants de writeback y, al activar chaining, wakeups de `ReadinessTable`.

### Envía

- Peticiones de lectura y escritura al VRF local.
- Bundles de ejecución a ALU, MUL y FPU.
- Paquetes a otras lanes por la interconexión.
- Actualizaciones opcionales de readiness después del writeback.
- `UnitCompletion` al distribuidor o sequencer.

La lane mantiene colas diferenciadas para tareas, solicitudes de operando,
operandos recibidos, operaciones en curso y writebacks pendientes.

Las colas de operandos se sitúan entre la salida del VRF y las unidades
funcionales. Las colas de resultados o writeback retienen los datos producidos
hasta que se concede la escritura. Ambas pertenecen al camino de ejecución
de la lane; no son colas internas de los bancos del VRF.

### ALU, MUL y FPU de la lane

**Conexiones directas.** Reciben `ExecutionBundle` exclusivamente de su
`AraLane` propietaria y devuelven `ExecutionResult` a esa misma lane; no se
conectan directamente al VRF, al sequencer ni a memoria.

Reciben un `ExecutionBundle` con identificadores, opcode, operandos, máscara y
rango de elementos. Devuelven un `ExecutionResult` con datos, rango de destino
e indicadores de excepción. No acceden directamente al VRF.

El baseline sólo necesita la suma entera de 32 bits en la ALU. MUL y FPU pueden
declararse como puertos o módulos inactivos, pero no condicionan la admisión ni
la finalización de la secuencia inicial.

## LaneRegisterFile, AddressMapper y bancos

### LaneRegisterFile

**Conexiones directas.** Recibe peticiones de `AraLane` y `AraVLSU`; en
ampliaciones también de SLDU y MASKU. Envía respuestas y confirmaciones al
solicitante original, consulta `AddressMapper` y accede a los bancos del VRF.

Recibe peticiones de lectura/escritura de lanes, VLSU, SLDU y MASKU. Devuelve
`ReadResponse`, `WriteAck` o `retry`, y comunica una escritura efectiva a
`ReadinessTable`. Toda petición incluye:

```text
taskKey, registerRef, byteOffset, size, byteEnable, requester
```

- `taskKey` permite atribuir la petición a su tarea y, a través de su
  `CommandKey`, al comando y contexto originales.
- `registerRef` selecciona el grupo arquitectónico que se lee o escribe.
- `byteOffset` y `size` delimitan el intervalo solicitado en bytes, relativo al
  inicio del grupo; el intervalo es `[byteOffset, byteOffset + size)`.
- `byteEnable` indica qué bytes del intervalo son efectivos, por ejemplo tras
  aplicar una máscara o en una escritura parcial.
- `requester` identifica a la lane, VLSU, SLDU o MASKU que emite la petición y
  permite al banco aplicar arbitraje y devolver la respuesta al origen.

### AddressMapper

**Conexiones directas.** Es un servicio de cálculo compartido que consultan
`TaskDistributor`, `LaneRegisterFile` y, cuando corresponda, `AraVLSU`.
Devuelve el mapeo de lane, banco y fila al módulo que lo consultó; no recibe
comandos de la CPU ni tareas de ejecución.

Recibe `registerRef`, desplazamiento de byte, tamaño, número de lanes y
bancos. Devuelve:

```text
laneId, bankId, row, byteOffsetInWord, byteEnable
```

- `laneId` designa la lane propietaria del fragmento de registro.
- `bankId` selecciona el banco local que almacena la palabra solicitada.
- `row` es el índice de fila dentro de ese banco.
- `byteOffsetInWord` señala el primer byte dentro de la palabra física.
- `byteEnable` es la máscara de bytes de esa palabra, ya recortada si el rango
  original empieza o termina en mitad de ella.

Es la única implementación de la regla que transforma una posición del
registro en lane, banco y fila. `TaskDistributor` usa esa regla para repartir
el trabajo y `LaneRegisterFile` para localizar los bytes de cada acceso.
Inicialmente se consulta como un servicio de cálculo sin cola ni latencia
propia; no representa un recurso central que serialice las lanes. El LRF
gestiona el arbitraje de acceso y los bancos tienen su latencia. Las colas de
operandos y writeback se sitúan en la lane, fuera de los bancos.

### Banco de VRF

**Conexiones directas.** Recibe accesos ya mapeados exclusivamente desde
`LaneRegisterFile`. Devuelve datos, confirmaciones o `retry` a
`LaneRegisterFile`, que los reenvía al solicitante original.

Recibe lecturas/escrituras ya mapeadas y una clase de solicitante. Devuelve
grant, datos tras su latencia, confirmación de escritura o retry. Una política
inicial puede arbitrar round-robin entre lecturas de operandos, writeback de
lanes, VLSU y SLDU/MASKU.

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

La secuencia inicial no necesita slide, gather ni reducción. La interconexión
sólo participa si el reparto de cargas o el acceso al VRF exige trasladar datos
entre la VLSU y la lane propietaria; en otro caso puede ser un enlace ideal.

## AraVLSU

**Conexiones directas.** Recibe `MemoryTask` de `AraSequencer`, datos de store
desde `LaneRegisterFile` y respuestas de `VectorMemoryBackend`. Envía
peticiones a `VectorMemoryBackend`, datos de carga a `AraLane` o al VRF y la
finalización a `AraSequencer`.

El baseline implementa únicamente `vle32.v` y `vse32.v` unit-stride. Genera uno
o varios accesos que cubren los elementos de 4 bytes del rango `[vstart, vl)` y
mantiene su asociación con `elementIndex`. No completa el comando hasta
escribir todas las cargas o recibir la confirmación de todos los stores.

### Recibe

- `MemoryTask` desde el sequencer.
- Datos para stores e índices desde el VRF.
- Respuestas de `VectorMemoryBackend`.
- Capacidad de writeback de las lanes propietarias.

### Envía a VectorMemoryBackend

```text
VectorMemoryRequest(taskKey, requestId, registerRef, elementIndex,
                    laneId, destinationByteRange, virtualAddress, size,
                    isLoad, isStore, storeData, byteEnable, orderingMetadata)
```

- `taskKey` identifica la tarea de memoria y `requestId` identifica de forma
  única esta subpetición entre las peticiones pendientes de esa tarea. La
  pareja constituye conceptualmente un futuro `RequestKey`.
- `registerRef` es el grupo de destino en una carga o el grupo fuente de los
  datos en un store: `VectorRegRef` en el baseline o `PhysicalRegRef`, con
  su versión, en el modo con renombramiento. `elementIndex` es el elemento
  vectorial al que corresponde y permite calcular `vstart` si ocurre un
  fallo.
- `laneId` es la lane propietaria del writeback de una carga. En un store puede
  conservarse como origen para trazabilidad y arbitraje.
- `destinationByteRange` es el intervalo del grupo de destino donde se
  escribirá una carga; no se usa para stores.
- `virtualAddress` es la dirección virtual ya calculada para este acceso, y
  `size` es el número de bytes que se transferirán.
- `isLoad` e `isStore` distinguen la dirección de la transferencia y son
  mutuamente excluyentes. `storeData` sólo es válido para un store.
- `byteEnable` precisa los bytes activos de una transferencia parcial.
- `orderingMetadata` queda en su valor neutro para las operaciones unit-stride
  iniciales. Sus variantes para accesos ordenados, segmentados o
  *fault-only-first* se reservan para ampliaciones posteriores.

### Envía a lanes o VRF

```text
LoadData(taskKey, requestId, destinationReg, elementIndex,
         laneId, destinationByteRange, data)
```

- `taskKey` y `requestId` correlacionan los datos con la petición original.
- `destinationReg` conserva la referencia de destino de la petición:
  arquitectónica en el baseline o física, con su versión, en el modo con
  renombramiento. `destinationByteRange` delimita exactamente dónde se deben
  escribir los bytes recibidos.
- `elementIndex` conserva la posición arquitectónica para actualizar
  readiness, diagnosticar fallos y completar tareas fragmentadas.
- `laneId` selecciona el propietario del writeback y `data` contiene los bytes
  de la respuesta, en el mismo orden que el rango de destino.

También notifica `UnitCompletion` y comunica un fault con el elemento causante
y el `vstart` resultante. Si se activa `ReadinessTable`, la actualiza después
de escribir una carga. La VLSU guarda una tabla de requests pendientes indexada
por `requestId`; las respuestas no se asocian por orden de llegada.

## VectorMemoryBackend

Es la única capa que conoce `Request`, `Packet`, TLB y puertos de memoria de
gem5.

**Conexiones directas.** Recibe `VectorMemoryRequest` de `AraVLSU` y eventos
de traducción, respuesta o `retry` de los puertos de gem5. Envía peticiones de
traducción y paquetes a gem5, y respuestas normalizadas a `AraVLSU`.

### Recibe

- `VectorMemoryRequest` de la VLSU.
- Señales de retry, reset o drain.
- Traducciones, respuestas y faults de gem5.

### Envía

- Peticiones de traducción y paquetes de carga/almacenamiento a gem5.
- Reenvío cuando gem5 señalice `recvReqRetry`.
- A la VLSU:

```text
VectorMemoryResponse(taskKey, requestId, status, data, fault)
```

- `taskKey` y `requestId` identifican la solicitud que el backend ha
  completado; la VLSU los usa como clave de su tabla de peticiones pendientes.
- `status` distingue una carga correcta, una confirmación de store, un fallo
  de traducción/acceso o una cancelación por reset o drain.
- `data` contiene los bytes de una carga correcta y está vacío en stores o
  respuestas con error.
- `fault` sólo está presente cuando `status` representa un fallo e incluye la
  causa y dirección necesarias para construir la excepción de gem5.

Así ningún objeto interno de memoria de gem5 se filtra a las lanes.

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

Las referencias de máscara del baseline deben usar `maskReg`, `firstBit` y
`bitCount`, en lugar de rangos de bytes. La extensión con renombramiento puede
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
  -> CpuVectorInterface asigna CommandKey y valida VectorCommand
  -> Commit obtiene grant y hace dispatch
  -> admisión almacena el comando y emite accepted
  -> AraSequencer asigna TaskKey y genera ArithmeticTask o MemoryTask
  -> lanes o VLSU ejecutan y escriben el resultado
  -> AraSequencer agrega finalizaciones y peticiones pendientes
  -> CpuVectorInterface emite completed
  -> Minor finaliza la instrucción o entrega el fault
```

La posible optimización futura de retirar la instrucción de Minor en
`accepted`, en vez de hacerlo en `completed`, debe posponerse hasta disponer de
un mecanismo explícito de excepciones precisas y orden de memoria entre la CPU
escalar y la VPU.

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
