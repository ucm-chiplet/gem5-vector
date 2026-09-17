# Cambios para integrar una VPU Ara-like en gem5 v25

## Alcance

El primer objetivo es integrar el coprocesador vectorial únicamente con
`MinorCPU`. No se modifica `O3CPU` en esta fase.

gem5 v25 conserva:

- Decodificación y semántica RVV 1.0.
- Estado arquitectónico `vl`, `vtype` y `vstart`.
- Pipeline escalar MinorCPU, scoreboard, TLB, cachés y memoria.
- Gestión de *squash* y de *control hazards* anteriores a una instrucción
  vectorial.

La nueva VPU será un backend desacoplado inspirado en Ara/AraXL. El código de
Vitruvius se reutilizará como referencia para la interfaz CPU–VPU, el control
de créditos y las estructuras de versiones físicas, pero no se portará
literalmente.

### Contratos del baseline y estado del plan

[[Interfaces propuestas]] es la referencia detallada de campos, invariantes,
propiedad de mensajes y condiciones de finalización. Este documento conserva
el plan de integración y sus responsabilidades, sin duplicar las estructuras
internas completas.

El baseline mantiene una instrucción activa, referencias arquitectónicas y
una petición lógica de memoria de un elemento de 32 bits en curso. Una carga
termina después de su writeback; un store, después de su confirmación. No se
inicia el elemento siguiente antes de cerrar el actual. Drain deja terminar
el trabajo admitido; el reset con trabajo activo queda fuera del primer hito.

Las tablas de archivos describen destinos del plan, no certifican que estén
implementados. Ya existen cabeceras básicas en `common/` e `interface/`, pero
los módulos del frontend y backend aún son armazones vacíos. Las nuevas
estructuras de tareas, fragmentos, VRF y memoria son contratos documentales
pendientes de implementación. Esta revisión no modifica el código.

### Configuración y propietarios del estado

`VectorEngine` obtiene los parámetros del SimObject, construye
`VpuParameters`, valida geometría, LMUL admitidos y capacidades y proporciona
vistas inmutables a sus módulos. `VectorConfig` sigue siendo el estado RVV
capturado por instrucción; no se usa como configuración del hardware.

| Propietario | Estructura o contrato | Responsabilidad |
| --- | --- | --- |
| `VectorEngine` | `VpuParameters` | Geometría compartida, capacidades en unidades explícitas y compatibilidad con Minor. |
| Decode/Execute de Minor | `DecodedVectorOp`, `MinorVectorState` | Conservar semántica decodificada, resolver operandos y asociar comando e instrucción hasta el cierre CPU. |
| Lane | `LaneFragmentState`, `PendingVrfAccess` | Relacionar lecturas de ambas fuentes, ejecución y writeback sin depender del orden de respuestas. |
| LRF y banco local | Mensajes `BankReadRequest`, `BankWriteRequest` y respuestas | El LRF mapea y arbitra; el banco aplica accesos a filas ya resueltas. |
| VLSU | `PendingVrfAccess` del elemento activo | Asociar lectura de store o escritura de carga con la petición lógica. |
| Backend de memoria | `MemoryTransactionState`, `MemoryFragmentState` | Traducción, fragmentación, paquetes, retry, buffers y liberación de objetos de gem5. |

Los campos, invariantes y validadores se detallan en [[Interfaces propuestas]].
Las estructuras privadas no requieren contenedores ni módulos nuevos por
defecto. La implementación elegirá sus contenedores manteniendo esas reglas.

Un mensaje interno que recibe `Retry` conserva identidad y datos; su emisor
programa una evaluación para el siguiente ciclo, con un solo evento de
reintento pendiente. Tras aceptación espera respuesta, y los callbacks
reactivan el trabajo habilitado. Un envío timing rechazado por el puerto de
memoria espera específicamente `recvReqRetry`; no se sondea cada ciclo.
Estas reglas aseguran progreso funcional sin fijar rendimiento.

### Raíces de código del plan

Para distinguir de forma explícita el origen y el destino del código, el
documento utiliza estas raíces:

| Raíz | Significado |
|---|---|
| `gem5_VPU/` | Árbol final de trabajo: gem5 v25 más las modificaciones de MinorCPU/RVV y todo el código nuevo de la VPU Ara-like. |
| `gem5_Vitruvius/` | Copia histórica de consulta. Sus archivos se rescatan como referencia o se adaptan; no son el destino del código final. |

Código de acciones:

- 🟦 **Crear**: archivo nuevo dentro de `gem5_VPU/`.
- 🟨 **Modificar**: archivo existente de gem5 v25, modificado dentro de `gem5_VPU/`.
- 🟩 **Rescatar**: archivo de `gem5_Vitruvius/` usado como referencia o adaptado.
- ⬜ **Consultar**: archivo usado sólo para conocer la semántica o diseño; no se transfiere código.

## 1. Evitar la descomposición RVV en microoperaciones

En MinorCPU, la descomposición de macroinstrucciones ocurre en:

```text
gem5_VPU/src/cpu/minor/decode.cc
```

Cuando el coprocesador esté habilitado, una instrucción RVV debe atravesar el
decode como una instrucción vectorial completa. La VPU debe recibir una única
orden vectorial, no las microoperaciones internas que utiliza el backend RVV
nativo de gem5.

La condición de bypass debe ser equivalente a:

```text
inst->isVector()
&& cpu.hasVectorCoprocessor()
&& vectorOffloadEnabled
```

La modificación debe mantener el comportamiento nativo de gem5 cuando
`vectorOffloadEnabled = false`.

Decode conserva la clasificación y semántica en `DecodedVectorOp`, asociado
a la macro. Las referencias vectoriales son números arquitectónicos; las
escalares son índices de operandos fuente que Execute resuelve mediante las
APIs de gem5 cuando están listos. Esta representación sólo vive en CPU.
Commit construye o entrega el comando capturado sin decodificar de nuevo.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/minor/decode.cc` | 🟨 Modificar: detectar el offload antes de expandir la macroinstrucción. | No; cambio nuevo sobre gem5 v25. |
| `gem5_VPU/src/arch/riscv/isa/formats/vector_arith.isa` y `vector_mem.isa` | ⬜ Consultar para preservar la instrucción RVV 1.0 completa; modificar sólo si Decode no basta. | No. |
| `gem5_Vitruvius/src/arch/riscv/isa/formats/vector.isa` | 🟩 Rescatar sólo como referencia del bypass antiguo. | No reutilizable directamente: RVV 0.7.1. |

## 2. Minor Execute: tratar la instrucción como operación desacoplada

En `MinorCPU::Execute`, una instrucción vectorial no debe buscar una FU
escalar ni invocar su ejecución funcional estándar.

La instrucción debe:

1. Comprobar dependencias escalares mediante el scoreboard de MinorCPU.
2. Marcar sus destinos escalares como no disponibles si corresponde.
3. Insertarse en `inFlightInsts`.
4. Avanzar hacia commit sin ejecutar `inst->staticInst->execute()`.

Esto reutiliza conceptualmente el mecanismo de Vitruvius en
`gem5_VPU/src/cpu/minor/execute.cc`: las instrucciones vectoriales se mantienen como
instrucciones en vuelo del pipeline escalar, pero su ejecución se delega a la
VPU.

Execute conserva `MinorVectorState`: instrucción, fase, descripción
decodificada, clave, comando capturado, token no consumido y finalización
pendiente, según la fase. `Stall` no recaptura operandos ni cambia identidad.
Las asociaciones CPU no cruzan hacia la VPU. El estado se prepara antes de
llamadas que puedan producir callbacks síncronos, y sólo se libera después
de procesar el resultado o fault.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/minor/execute.cc` | 🟨 Modificar: detectar el comando vectorial, usar el scoreboard e insertarlo en `inFlightInsts` sin usar una FU escalar. | Sí, como patrón a portar y adaptar. |
| `gem5_VPU/src/cpu/minor/execute.hh` | 🟨 Modificar si se requieren estados, IDs o callbacks de VPU. | No directamente. |
| `gem5_Vitruvius/src/cpu/minor/execute.cc` | 🟩 Rescatar y adaptar la lógica vectorial existente en Minor. | Sí, pero no copiar literalmente: usa APIs y RVV antiguos. |

## 3. Commit: envío a la VPU

La instrucción vectorial se envía cuando alcanza commit. En ese punto ya no
puede ser anulada por una instrucción escalar anterior.

El protocolo mínimo será:

```text
MinorCPU -> VPU: requestGrant(command)
VPU      -> CPU: grant(token) / stall / rejected
MinorCPU -> VPU: dispatch(token, command)
VPU      -> CPU: accepted(CommandKey)
VPU      -> CPU: completed(VectorCompletion)
```

Se deben distinguir los resultados de admisión y los eventos de ciclo de vida:

- `grant`: la VPU reserva capacidad y devuelve un `GrantToken`.
- `stall`: no hay capacidad temporalmente y MinorCPU puede reintentar.
- `rejected`: el descriptor es inválido o no está soportado; no se reintenta.

- `accepted`: la VPU ha almacenado la orden en su cola.
- `completed`: la ejecución vectorial ha terminado.

No se debe usar un único callback para ambos casos, ya que Vitruvius mezcla
aceptación y finalización en algunos caminos.

El token contiene el `CommandKey` asociado y un identificador opaco de
reserva. Sólo puede consumirse una vez y únicamente con el comando para el que
se concedió. De este modo, la capacidad reservada por `requestGrant` no puede
desaparecer antes de `dispatch`.

Las instrucciones `vsetvl`/ `vsetvli` se procesan en el lado CPU para
actualizar el estado RVV 1.0 de gem5. La VPU recibe después el valor efectivo
de `vl`, `vstart`, `SEW`, `LMUL`, máscara y políticas de tail y mask. Recibe
estos campos ya normalizados y no vuelve a decodificar `vtype`.

### Coordinación de drain con Minor

Drain detiene los grants nuevos y los reintentos de admisión. Una instrucción
sin token ni despacho puede descartarse mediante el vaciado normal de Minor,
limpiando dependencias y conservando el punto arquitectónico para volver a
buscarla al reanudar. Su identidad anterior no se reutiliza.

Una reserva concedida se consume y un comando despachado o aceptado debe
terminar. Se protege su estado frente al descarte de instrucciones, se siguen
atendiendo callbacks y se procesa también cualquier fault terminal. No se
genera `Cancelled` ni se reejecuta al reanudar un comando ya completado.

La implementación deberá coordinar `Execute::drain`, `isInbetweenInsts`,
`isDrained` y `DrainAllInsts` con el estado de offload. El criterio conjunto
incluye pipeline y LSQ de Minor, reservas, tareas, respuestas, writebacks y
callbacks/eventos con trabajo de la VPU. No basta con vaciar `CommandQueue`.
Los detalles y la tabla por fase están en [[Interfaces propuestas]].

### Archivos implicados y reutilización

| Archivo                                                               | Acción                                                               | Vitruvius necesario                                      |
| --------------------------------------------------------------------- | -------------------------------------------------------------------- | -------------------------------------------------------- |
| `gem5_VPU/src/cpu/minor/execute.cc`                                   | 🟨 Modificar: emitir `requestGrant` y `dispatch` al llegar a commit. | Sí, como patrón de integración.                          |
| `gem5_VPU/src/cpu/vector_engine/interface/cpu_vector_interface.hh/.cc` | 🟦 Crear: identidad, admisión, despacho y callbacks separados.       | No; desarrollo propio.                                 |
| `gem5_Vitruvius/src/cpu/vector_engine/vector_engine_interface.hh/.cc` | 🟩 Rescatar y adaptar el patrón `requestGrant`/`sendCommand`.        | Sí, como referencia; no se porta su tipo de instrucción. |
| `gem5_Vitruvius/src/cpu/vector_engine/vector_engine.cc`               | 🟩 Rescatar como referencia para la concesión de recursos.           | Opcional; sólo referencia.                               |

## 4. Descriptor para comunicación entre CPU y VPU

No se pasarán punteros a las clases RVV 0.7.1 de Vitruvius. MinorCPU extrae la
semántica y los operandos de RVV 1.0, solicita a `CpuVectorInterface` un
`CommandKey` y construye un descriptor propio. La interfaz lo valida y lo
transporta sin volver a decodificar la instrucción.

El descriptor no contiene un `VectorOpcode` por cada mnemónico. Se compone a
partir de operaciones semánticas y payloads tipados:

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

`vadd.vv` y `vadd.vx` comparten `ArithmeticOperation::Add`; la alternativa de
`ArithmeticOperand` indica si el segundo operando es un registro vectorial, un
valor escalar o un inmediato. `vle32.v` y `vse32.v` utilizan
`MemoryCommand`, con dirección `Load` o `Store`, EEW de 32 bits y
`UnitStrideAddress`.

La unidad receptora se deriva del payload y no se almacena como un campo
duplicado. `VectorConfig` contiene los valores efectivos de `vl`, `vstart`,
SEW, LMUL, máscara y políticas tail/mask agnostic ya calculados por la CPU.
`CommandKey` conserva `commandId` y `contextId`, y `requestorId` identifica al
solicitante de memoria en modo SE.

`VectorCommand` será la única interfaz entre MinorCPU y la VPU. Las clases
internas de la VPU no deben depender de `StaticInst` ni volver a decodificar la
instrucción.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_engine/common/command_key.hh` | 🟦 Crear: identidad completa del comando. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/common/vector_reg_ref.hh` | 🟦 Crear: referencias a grupos arquitectónicos. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/common/vector_types.hh` | 🟦 Crear: configuración y vocabulario semántico compartido. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/interface/vector_command.hh` | 🟦 Crear: descriptor tipado e inmutable durante el despacho. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/interface/vector_completion.hh` | 🟦 Crear: estado, resultado escalar, `vstart` y fault. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/interface/cpu_vector_interface.hh/.cc` | 🟦 Crear: asignación de identidad, validación y transporte. | No; desarrollo propio. |
| `gem5_VPU/src/arch/riscv/insts/vector.hh` y formatos RVV de gem5 v25 | ⬜ Consultar para extraer semántica, operandos y estado RVV 1.0. | No; gem5 v25 es la autoridad. |
| `gem5_Vitruvius/src/cpu/vector_engine/vector_dyn_inst.hh` | 🟩 Rescatar sólo como referencia para distinguir estado estático y dinámico. | Opcional; no reutilizable directamente. |

## 5. VPU Ara-like con una cola única

La VPU no usará las dos colas de Vitruvius (`Instruction_Queue` y
`Memory_Queue`) como arquitectura objetivo.

```text
VectorCommand FIFO
        |
        v
AraSequencer
 ├── lanes
 ├── VLSU
 ├── SLDU
 └── MASKU
```

La FIFO conserva el orden de llegada de los comandos. El `AraSequencer` mira
la instrucción situada en la cabeza y la distribuye a la unidad correspondiente:

- aritmética → lanes;
- loads/stores → VLSU;
- slides, gathers y permutaciones → SLDU;
- máscaras → MASKU;
- reducciones → lanes + interconexión.

Al dividir un comando, `AraSequencer` asigna un `TaskId` único dentro de su
`CommandKey` y forma un `TaskKey`. Todas las tareas especializadas contienen
un descriptor común `UnitTask` con ese `TaskKey`, la clase de unidad y el
intervalo lógico de elementos. Los rangos de bytes no pertenecen al descriptor
común: `ArithmeticTask` define su rango de destino y `MemoryTask` un
`dataRange`, que es destino para loads y fuente para stores. Ambas conservan
configuración y payload; `MemoryTask` conserva además `pc` y `requestorId`.

El baseline genera una tarea por comando no vacío. `TaskDistributor` divide
la tarea aritmética en fragmentos contiguos de una palabra como máximo,
identificados por `LaneFragmentKey = (TaskKey, LaneFragmentId)`. El segundo
operando conserva su alternativa vectorial o escalar; no se pierde el valor
de `vadd.vx` al construir un `LaneTask`.

Las lanes devuelven `LaneCompletion` después del writeback. El distribuidor
agrega los fragmentos y devuelve `UnitCompletion`; la VLSU devuelve ese mismo
tipo al cerrar su tarea. El sequencer produce un único `VectorCompletion`.
Un rango vacío no genera tareas ni accesos y completa con `finalVstart=0`.
Reservas, tareas y fragmentos pendientes son estado de control, no un ROB.

La cola única debe ser parametrizable en profundidad, pero inicialmente tendrá
política FIFO estricta. No se implementará issue OoO entre instrucciones en esta
primera fase.

```text
enable_ara_fifo = true
issue_policy = in_order
```

Esto acerca el modelo al estilo Ara y evita reutilizar accidentalmente la
política OoO de las colas separadas de Vitruvius.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_engine/frontend/ara_sequencer.hh/.cc` | 🟦 Crear: scheduler de cola única FIFO y distribución hacia unidades Ara. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/frontend/command_queue.hh/.cc` | 🟦 Crear: FIFO de `VectorCommand`. | No; desarrollo propio. |
| `gem5_Vitruvius/src/cpu/vector_engine/vector_engine.cc` | 🟩 Rescatar como referencia para el patrón de `dispatch`. | Útil como referencia. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/issue_queues/inst_queue.hh/.cc` | 🟩 Rescatar sólo accounting, capacidad y trazas. | No reutilizar directamente: tiene dos colas y `OoO_queues`. |

## 6. Renombramiento configurable

Esta sección describe una ampliación posterior. Las estructuras se reservan
bajo `future/` y permanecen inactivas durante el baseline.

El renombramiento será independiente de la política de issue:

```text
enable_vector_renaming = false | true
issue_policy            = in_order
```

### Modo sin renombramiento

La VPU usa directamente registros vectoriales arquitectónicos.

- No existe RAT ni free-list.
- No existe ROB vectorial.
- Las dependencias RAW, WAR y WAW se conservan de forma natural.
- La FIFO y el sequencer ejecutan las instrucciones en orden.
- Es el baseline Ara-like más simple.

### Modo con renombramiento

La VPU añade estructuras inspiradas en Vitruvius:

```text
RAT + Free Register List + Vector ROB
```

El renombramiento se realiza al aceptar el comando en la VPU:

1. Las fuentes lógicas se traducen mediante la RAT.
2. El destino recibe una versión física nueva desde la free-list.
3. El destino físico anterior se guarda en el ROB.
4. La RAT se actualiza con la nueva versión.
5. El destino anterior se libera cuando la entrada correspondiente se retira.

El ROB no controla el issue ni activa OoO. Sólo conserva versiones físicas y
libera el destino antiguo en orden.

El beneficio inicial del modo con renaming es poder mantener varias versiones
físicas de registros en vuelo. La exploración de OoO se deja como extensión
posterior; no debe introducirse al mismo tiempo que la primera VPU Ara-like.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_engine/future/rename.hh/.cc` | 🟦 Crear o reescribir: RAT, free-list y soporte ON/OFF para RVV 1.0. | Sí, como modelo conceptual. |
| `gem5_VPU/src/cpu/vector_engine/future/reorder_buffer.hh/.cc` | 🟦 Crear o reescribir: retire ordenado de versiones físicas. | Sí, como modelo conceptual. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/rename/vector_rename.hh/.cc` | 🟩 Rescatar como referencia para RAT/FRL. | No reutilizable directamente: no cubre correctamente LMUL/EMUL RVV 1.0. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/rob/reorder_buffer.hh/.cc` | 🟩 Rescatar como referencia para liberar `old_dst` en orden. | Adaptable conceptualmente; debe reescribirse. |

## 7. Desarrollo de lanes reales

`VectorLane` del fork de Vitruvius no se reutilizará como lane física final:
representa un clúster agregado y calcula varios elementos por tick.

La nueva VPU debe crear lanes explícitas:

```text
AraLane
├── cola local de tareas
├── operand requester
├── operand queues
├── slice local del VRF
├── ALU / MUL / FPU
├── writeback
└── interfaz de interconexión
```

Cada lane procesa solamente las palabras que le corresponden. El mapeo
incluye el registro arquitectónico, además del desplazamiento relativo:

```text
absolute_byte = first_reg * vlen_bytes + byte_offset
global_word   = absolute_byte / lane_word_bytes
lane_id       = global_word % num_lanes
local_word    = global_word / num_lanes
bank_id       = local_word % banks_per_lane
row           = local_word / banks_per_lane
```

`VrfGeometry` comparte VLEN, anchura de palabra, número de lanes y bancos.
Las restricciones de geometría y `MappedVrfFragment` se definen en
[[Interfaces propuestas]]. Un rango puede producir varios fragmentos; el
reparto no representa elementos dispersos como un único intervalo contiguo.

Esta regla se implementa una sola vez en `AddressMapper`.
`TaskDistributor` consulta el servicio para repartir el trabajo;
`LaneRegisterFile` y `AraVLSU` lo consultan para localizar los bytes. No tiene
cola ni latencia propia; no es un recurso central que serialice las lanes.
Cada lane convierte sus operandos a un `ExecutionBundle` de valores de 32 bits
y recibe un `ExecutionResult` antes del writeback. No se fijan en esta fase
las latencias o el throughput de la ALU.

`LaneFragmentState` conserva operandos, fase de ejecución y resultado hasta
el writeback. `PendingVrfAccess` relaciona cada acceso con su fragmento y su
papel: primera fuente, segunda fuente o destino. Las dos lecturas se asocian
por identidad aunque sus respuestas cambien de orden. Se reserva recepción
antes de emitir; la finalización requiere consumir el `WriteAck`.

El número de lanes debe cambiar el paralelismo real y no únicamente la cantidad
de elementos calculados en una llamada al datapath.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_engine/vpu/lanes/ara_lane.hh/.cc` | 🟦 Crear: lane física, colas locales, arbitraje y progreso independiente. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/frontend/task_distributor.hh/.cc` | 🟦 Crear: reparto de palabras o tareas a lanes. | No; desarrollo propio. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/multilane_wrapper/vector_lane.hh/.cc` | 🟩 Rescatar sólo readers, writers y ciclo de vida de una operación. | No reutilizar directamente: modela un clúster agregado. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/multilane_wrapper/datapath.hh/.cc` | 🟩 Rescatar sólo operaciones y latencias de referencia. | No reutilizar directamente: no son pipelines ni lanes físicas independientes. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/multilane_wrapper/func_unit.hh` | 🟩 Rescatar como inventario de operaciones. | Opcional; validar contra RVV 1.0. |

## 8. VRF e interconexión

El `VectorRegister` central de Vitruvius puede reutilizarse temporalmente como
almacenamiento funcional, pero no como modelo final de timing.

La arquitectura objetivo requiere:

- una slice local del VRF por lane;
- bancos y puertos configurables;
- arbitraje entre lectura de operandos, writeback y VLSU;
- latencia por banco y arbitraje de acceso, sin FIFO de peticiones por banco;
- colas de operandos entre el VRF y las unidades funcionales, y colas de
  writeback para retener resultados hasta obtener acceso al banco;
- distribución de datos de cargas hacia la lane propietaria;
- interconexión entre lanes para slides, gathers y reducciones.

El modelo inicial puede utilizar una interconexión ideal. Ring, crossbar o la
topología exacta de AraXL se añadirán cuando se confirme la referencia hardware.

Las colas de operandos y writeback pertenecen al camino de ejecución de cada
lane. Si un acceso pierde el arbitraje, el solicitante lo mantiene pendiente
hasta obtener concesión; el banco no encola la petición. Antes de emitir una
lectura se comprueba que su cola de operandos pueda recibir la respuesta.
La frontera LRF--banco conserva `VrfAccessKey`, fila, máscara de palabra y
datos de escritura. El LRF expande o compacta el rango original y recibe
`BankReadResponse` o `BankWriteAck`; el banco no reconstruye grupos vectoriales
ni añade una FIFO. Los formatos se definen en [[Interfaces propuestas]].

Esta organización sigue el
[VRF de Ara](https://pulp-platform.github.io/ara/modules/lane/vrf.html).

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_engine/vpu/register_file/lane_register_file.hh/.cc` | 🟦 Crear: slice local de VRF por lane. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/vpu/register_file/address_mapper.hh/.cc` y `vector_reg_bank.hh/.cc` | 🟦 Crear: mapeo de palabra a lane/banco y arbitraje temporizado. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/vpu/interconnect/vector_interconnect.hh/.cc` | 🟦 Crear: interconexión ideal inicialmente y topologías posteriores. | No; desarrollo propio. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/register_file/vector_reg.hh/.cc` | 🟩 Rescatar sólo almacenamiento funcional y puertos actuales. | No reutilizar como timing final: es central y multiport. |

### Reutilización de la memoria de Vitruvius

La nueva VLSU consultará Vitruvius como referencia para patrones de acceso y
requests temporizadas. Sólo unit-stride está activo en el baseline. Los casos
de prueba de la VMU pueden servir como referencia al equipo humano; este plan
no autoriza su portado automático.

No se reutilizarán directamente `VectorMemUnit`, `MemUnitReadTiming` ni
`MemUnitWriteTiming`, porque dependen de RVV 0.7.1 y de un VRF central. Se
usarán como referencia de diseño para crear una `AraVLSU` conectada a los
puertos de memoria de gem5 v25.

Cada `VectorMemoryRequest` conserva:

```text
taskKey + requestId
registerRef + dataRange + elementIndex + laneId
virtualAddress + size + direction
storeData + byteEnable
pc + requestorId
```

`direction` es `MemoryDirection`, sin booleanos independientes.
`registerRef` es arquitectónico; `dataRange` es destino en cargas y fuente en
stores. La clave completa incluye el contexto de `TaskKey`. `RequestId` no
se reutiliza dentro de una tarea. Una ampliación podrá formalizar `RequestKey`,
pero no es necesario introducirlo ahora.

La VLSU mantiene el elemento activo hasta el `WriteAck` del VRF en una carga
o `StoreAck` de memoria en un store. Su acceso al VRF se identifica mediante
`VrfAccessKey`, distinto de la identidad de memoria, y se dirige al LRF de la
lane propietaria. `LoadData` conserva la referencia y el rango originales
mientras espera escritura.

`VectorMemoryBackend` mantiene los metadatos de contexto para traducción y
paquetes. Si el acceso requiere fragmentos físicos, conserva internamente sus
desplazamientos y respuestas, sin crear peticiones lógicas nuevas. Se encarga
de `recvReqRetry` después de aceptar la petición de la VLSU. Devuelve una
única `VectorMemoryResponse`: datos completos de carga, confirmación de store
o fault. No se publica una carga parcial en el VRF.

En el baseline no hay varias peticiones lógicas en vuelo. Los elementos se
procesan en orden y un fault cierra la petición actual sin emitir el siguiente.
La dirección y el índice causantes llegan al sequencer, que devuelve
`MemoryFault` y `finalVstart` correspondiente. Los errores internos de
identidad o protocolo se diagnostican como errores del simulador. Drain no
cancela accesos admitidos. Cargas solapadas, cancelación activa y chaining
requieren una ampliación posterior del estado de seguimiento.

`MemoryTransactionState` conserva la petición lógica, buffer, fragmentos,
traducciones pendientes y fallo terminal. Cada `MemoryFragmentState` conserva
direcciones, rango y fase. Las referencias usadas por callbacks y paquetes
permanecen válidas hasta cerrar el acceso. Un paquete no aceptado sigue
perteneciendo al backend; uno aceptado no se modifica ni se destruye en vuelo.
La respuesta permite consumir datos, retirar el estado de retorno y liberar
el paquete. La transacción se libera sólo tras cerrar los callbacks y emitir
la única respuesta lógica. La tabla de propiedad está en
[[Interfaces propuestas]].

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_engine/vpu/vlsu/ara_vlsu.hh/.cc` | 🟦 Crear: VLSU con requests identificadas y distribución por lane. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/vpu/vlsu/vector_memory_backend.hh` | 🟦 Crear: frontera entre VLSU y memoria gem5. | No; desarrollo propio. |
| `gem5_Vitruvius/src/cpu/vector_engine/vmu/vector_mem_unit.hh/.cc` | 🟩 Rescatar patrones unit-stride, strided e indexed. | Sí, como referencia funcional y de pruebas; no como código directo. |
| `gem5_Vitruvius/src/cpu/vector_engine/vmu/read_timing_unit.hh/.cc` y `write_timing_unit.hh/.cc` | 🟩 Rescatar requests temporizadas y callbacks. | Adaptables conceptualmente; modernizar y desacoplar del VRF central. |

## 9. Chaining y valid bits granulares

El `VectorValidBit` de Vitruvius tiene un único bit por registro físico. Esto
no permite chaining: el consumidor sólo puede comenzar cuando el productor ha
terminado el registro completo.

Aunque el chaining no forma parte de las primeras fases, la nueva VPU debe
reservar desde el diseño una tabla de readiness asociada a cada **versión
física** del registro. La representación recomendada en la documentación del
proyecto es por bytes:

```text
ready_bytes[physical_version][0 .. VLEN_bytes - 1]
mask_ready_bits[physical_mask_version][0 .. VLEN_bits - 1]
```

Un elemento de `k` bytes podrá considerarse listo cuando todos sus bytes estén
disponibles. Esta granularidad evita perder información si el mismo registro se
produce con un EEW y se consume con otro SEW/EEW. Las máscaras requieren una
tabla independiente a nivel de bit.

En las primeras fases sólo se implementará readiness a nivel de instrucción o
de registro completo. La tabla granular se añadirá como infraestructura para
una futura implementación de chaining. El mecanismo concreto de propagación
de datos queda abierto: podrá utilizar el LRF, colas de operandos, bypass u
otra estructura Ara/AraXL, pero no se fija todavía una política concreta de
propagación.

```text
productor y versión física
        |
        v
actualizar disponibilidad granular
        |
        v
despertar tareas consumidoras
```

La posición exacta en la que se actualizará la disponibilidad granular deberá
definirse junto con el modelo temporal de las lanes, el VRF y las colas de
operandos.

Cuando se implemente el chaining, se podrán evaluar al menos estos modos:

```text
chaining_mode = off | granular
```

- `off`: el destino se publica al completar todo el registro o grupo.
- `granular`: la disponibilidad se gestiona por bytes o elementos según la
  política que se defina para Ara/AraXL.

Puede existir un modo rápido con granularidad `lane_word` o `chunk`, pero
debe ser explícitamente configurable. No debe presentarse como chaining exacto
a nivel de elemento. La referencia de máxima precisión será la readiness por
bytes.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_engine/future/readiness_table.hh/.cc` | 🟦 Crear: readiness por bytes para versiones físicas y por bits para máscaras. | No; desarrollo propio. |
| `gem5_VPU/src/cpu/vector_engine/frontend/task_distributor.hh/.cc` y `ara_lane.hh/.cc` | 🟨 Modificar después: consultas y despertadores de disponibilidad granular. | No en las primeras fases. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/register_file/vector_reg_valid_bit.hh/.cc` | 🟩 Rescatar sólo para entender el límite del valid bit por registro. | No reutilizar directamente: su granularidad es insuficiente. |

## 10. Orden de implementación

La primera etapa es documental; las siguientes requieren sus propuestas de
código y aprobación correspondientes. No se implementan pruebas por defecto.

1. Usar los contratos de [[Interfaces propuestas]]: configuración global,
   representación CPU, tareas, mensajes, estados privados y reglas de progreso
   y drain. Las estructuras documentadas no se consideran ya implementadas.
2. Implementar `VpuParameters` y los tipos pendientes; completar validadores
   según su matriz de responsabilidades. Alinear el comentario y validación de
   `VectorCompletion` con los campos obligatorios de `MemoryFault`.
3. Implementar identidad, reservas, FIFO y protocolo
   `requestGrant`/`dispatch`/`accepted`/`completed`. Preparar reactivación por
   eventos y cierre de trabajo pendiente durante drain.
4. Integrar `DecodedVectorOp` y `MinorVectorState`, con `vsetvli` en CPU
   y la ruta nativa con offload deshabilitado. Coordinar descarte, callbacks,
   faults y drain de Minor con el frontend antes de ejecutar comandos.
5. Implementar `AraSequencer`, una tarea por comando no vacío y terminación
   inmediata de rangos vacíos.
6. Implementar geometría compartida, `AddressMapper`, VRF por lane y contrato
   LRF--banco, con lecturas, escrituras, confirmaciones y retry identificados.
7. Implementar `TaskDistributor`, fragmentos contiguos, ALU de 32 bits y
   agregación de `LaneCompletion` después de los writebacks. Usar
   `LaneFragmentState` y `PendingVrfAccess` para correlacionar los operandos.
8. Implementar VLSU y backend de memoria con un elemento en curso, acceso
   directo al LRF, traducción, fragmentación física, retry y faults. Aplicar
   las reglas de propiedad de transacciones, fragmentos, paquetes y buffers.
9. Validar el baseline SE con la secuencia y las pruebas definidas por el
   equipo humano; no evaluar rendimiento a partir del modelo funcional.
10. Tras terminar el baseline y con aprobación específica, ampliar el modelo
    temporal, el solapamiento de memoria, SLDU, MASKU e interconexión.
11. Incorporar renombramiento, ROB, readiness o chaining únicamente como
    ampliaciones aprobadas. Evaluar OoO o bypass en fases posteriores.

## 11. Código de Vitruvius reutilizable

Se puede reutilizar como referencia:

- `VectorEngineInterface`: patrón de comunicación CPU–VPU.
- Integración en `MinorCPU::Execute`.
- `requestGrant`: control de capacidad y backpressure.
- `VectorRename`: idea de RAT y free-list.
- `ReorderBuffer`: liberación en orden de versiones antiguas.
- VMU: clasificación de accesos unit-stride, strided e indexed.
- Configuración Python de estructuras vectoriales.

No se reutilizará literalmente:

- RVV 0.7.1 y `VectorStaticInst`.
- Colas separadas aritmética/memoria.
- Política `OoO_queues`.
- `VectorLane`/ `Datapath` como modelo de lane real.
- VRF central multiport.
- Fórmulas de latencia como sustituto de una interconexión.
- Valid bits por registro completo como modelo de chaining.

## 12. Benchmark inicial de validación funcional

Por ahora sólo habrá un benchmark inicial. Se ejecutará en modo **Syscall
Emulation (SE)** y su objetivo será comprobar que el flujo básico de
instrucciones RVV funciona correctamente desde la CPU hasta la VPU y produce el
resultado arquitectónico esperado.

El benchmark se ejecutará mediante la infraestructura global de `benchmarks/`,
independiente de `.ia` y de cualquier agente. El punto de entrada seguirá siendo:

```bash
./benchmarks/run_benchmarks.sh
```

La secuencia de instrucciones será la que ya se estableció en el conjunto de
referencia del proyecto, en este orden:

1. `vsetvli`: configuración de `vl`, `vtype`, `SEW` y `LMUL`.
2. `vle32.v`: carga vectorial de enteros de 32 bits desde memoria.
3. `vadd.vv`: suma vectorial elemento a elemento entre dos registros vectoriales.
4. `vadd.vx`: suma vectorial entre un registro vectorial y un escalar de la CPU.
5. `vse32.v`: almacenamiento del resultado vectorial en memoria.

La validación inicial será exclusivamente funcional. Debe comprobar que:

- el programa RVV arranca y termina correctamente en modo SE;
- `vsetvli` actualiza la configuración vectorial esperada;
- `vle32.v` lee los datos correctos desde memoria;
- `vadd.vv` y `vadd.vx` producen los valores esperados;
- `vse32.v` escribe el resultado final en la dirección prevista;
- el resultado almacenado coincide con el resultado de referencia.

El benchmark debe devolver un código distinto de cero si falla cualquiera de
estas comprobaciones. Las validaciones temporales, los conflictos de recursos,
el chaining y el resto de benchmarks se añadirán después de que este flujo
funcional pase de forma estable.

La propuesta de esta secuencia procede del documento [[Plan de Trabajo, Hitos y Matriz Experimental]] y coincide con el flujo RVV indicado en [[Ara2 - Arquitectura y Plan de Réplica en gem5]].
