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
VPU      -> CPU: grant / stall
MinorCPU -> VPU: dispatch(command)
VPU      -> CPU: accepted(command_id)
VPU      -> CPU: completed(command_id, status, scalar_result)
```

Se deben distinguir dos eventos:

- `accepted`: la VPU ha almacenado la orden en su cola.
- `completed`: la ejecución vectorial ha terminado.

No se debe usar un único callback para ambos casos, ya que Vitruvius mezcla
aceptación y finalización en algunos caminos.

Las instrucciones `vsetvl`/ `vsetvli` se procesan en el lado CPU para
actualizar el estado RVV 1.0 de gem5. La VPU recibe después el valor efectivo
de `vl`, `vtype`, `SEW`, `LMUL`, máscara y resto de metadatos.

### Archivos implicados y reutilización

| Archivo                                                               | Acción                                                               | Vitruvius necesario                                      |
| --------------------------------------------------------------------- | -------------------------------------------------------------------- | -------------------------------------------------------- |
| `gem5_VPU/src/cpu/minor/execute.cc`                                   | 🟨 Modificar: emitir `requestGrant` y `dispatch` al llegar a commit. | Sí, como patrón de integración.                          |
| `gem5_VPU/src/cpu/vector_backend/cpu_vector_interface.hh/.cc`         | 🟦 Crear: interfaz con `accepted` y `completed` separados.           | No existe; debe crearse.                                 |
| `gem5_Vitruvius/src/cpu/vector_engine/vector_engine_interface.hh/.cc` | 🟩 Rescatar y adaptar el patrón `requestGrant`/`sendCommand`.        | Sí, como referencia; no se porta su tipo de instrucción. |
| `gem5_Vitruvius/src/cpu/vector_engine/vector_engine.cc`               | 🟩 Rescatar como referencia para la concesión de recursos.           | Opcional; sólo referencia.                               |

## 4. Descriptor para comunicación entre CPU y VPU

No se pasarán punteros a las clases RVV 0.7.1 de Vitruvius. Se creará un
descriptor propio basado en RVV 1.0:

```cpp
struct VectorCommand
{
    uint64_t id;
    Addr pc;
    VectorOpcode opcode;
    VectorUnitClass unit;

    uint8_t vd, vs1, vs2, vs3;
    RegVal scalar_operand;

    uint32_t vl;
    uint32_t vstart;
    uint16_t sew_bits;
    Lmul lmul;

    bool masked;
    bool tail_agnostic;
    bool mask_agnostic;

    MemoryAddressMode memory_mode;
    Addr base;
    RegVal stride;
};
```

`VectorCommand` será la única interfaz entre MinorCPU y la VPU. Las clases
internas de la VPU no deben depender de `StaticInst` ni volver a decodificar la
instrucción.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_backend/vector_command.hh` | 🟦 Crear: definición estable del descriptor. | No existe; debe crearse. |
| `gem5_VPU/src/cpu/vector_backend/cpu_vector_interface.cc` | 🟦 Crear: extracción de datos RVV 1.0 hacia `VectorCommand`. | No existe; debe crearse. |
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
| `gem5_VPU/src/cpu/vector_backend/frontend/ara_sequencer.hh/.cc` | 🟦 Crear: scheduler de cola única FIFO y distribución hacia unidades Ara. | No existe; debe crearse. |
| `gem5_VPU/src/cpu/vector_backend/frontend/command_queue.hh/.cc` | 🟦 Crear: FIFO de `VectorCommand`. | No existe; debe crearse. |
| `gem5_Vitruvius/src/cpu/vector_engine/vector_engine.cc` | 🟩 Rescatar como referencia para el patrón de `dispatch`. | Útil como referencia. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/issue_queues/inst_queue.hh/.cc` | 🟩 Rescatar sólo accounting, capacidad y trazas. | No reutilizar directamente: tiene dos colas y `OoO_queues`. |

## 6. Renombramiento configurable

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
| `gem5_VPU/src/cpu/vector_backend/frontend/rename.hh/.cc` | 🟦 Crear o reescribir: RAT, free-list y soporte ON/OFF para RVV 1.0. | Sí, como modelo conceptual. |
| `gem5_VPU/src/cpu/vector_backend/frontend/reorder_buffer.hh/.cc` | 🟦 Crear o reescribir: retire ordenado de versiones físicas. | Sí, como modelo conceptual. |
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

Cada lane procesa solamente las palabras que le corresponden:

```text
global_word = byte_offset / lane_word_bytes
lane_id     = global_word % num_lanes
local_word  = global_word / num_lanes
```

Esta regla se implementa una sola vez en `AddressMapper`.
`TaskDistributor` consulta ese servicio para repartir el trabajo;
`LaneRegisterFile` lo consulta para localizar los bytes en lane, banco y
fila. `AraVLSU` también puede consultarlo cuando necesite determinar la
lane propietaria. Inicialmente es un servicio de cálculo compartido sin
cola ni latencia propia; las latencias de acceso se modelan en el VRF y sus
bancos.

El número de lanes debe cambiar el paralelismo real y no únicamente la cantidad
de elementos calculados en una llamada al datapath.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_backend/lanes/ara_lane.hh/.cc` | 🟦 Crear: lane física, colas locales, arbitraje y progreso independiente. | No existe; debe crearse. |
| `gem5_VPU/src/cpu/vector_backend/lanes/task_distributor.hh/.cc` | 🟦 Crear: reparto de palabras o tareas a lanes. | No existe; debe crearse. |
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
- latencia y cola por banco;
- distribución de datos de cargas hacia la lane propietaria;
- interconexión entre lanes para slides, gathers y reducciones.

El modelo inicial puede utilizar una interconexión ideal. Ring, crossbar o la
topología exacta de AraXL se añadirán cuando se confirme la referencia hardware.

### Archivos implicados y reutilización

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_backend/vrf/lane_register_file.hh/.cc` | 🟦 Crear: slice local de VRF por lane. | No existe; debe crearse. |
| `gem5_VPU/src/cpu/vector_backend/vrf/address_mapper.hh/.cc` y `bank.hh/.cc` | 🟦 Crear: mapeo de palabra a lane/banco y arbitraje temporizado. | No existe; debe crearse. |
| `gem5_VPU/src/cpu/vector_backend/noc/vector_interconnect.hh/.cc` | 🟦 Crear: interconexión ideal inicialmente y topologías posteriores. | No existe; debe crearse. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/register_file/vector_reg.hh/.cc` | 🟩 Rescatar sólo almacenamiento funcional y puertos actuales. | No reutilizar como timing final: es central y multiport. |

### Reutilización de la memoria de Vitruvius

La nueva VLSU reutilizará de Vitruvius la clasificación de patrones de acceso
(`unit-stride`, `strided` e `indexed`), el patrón de requests temporizadas y los
casos de prueba de la VMU.

No se reutilizarán directamente `VectorMemUnit`, `MemUnitReadTiming` ni
`MemUnitWriteTiming`, porque dependen de RVV 0.7.1 y de un VRF central. Se
usarán como referencia de diseño para crear una `AraVLSU` conectada a los
puertos de memoria de gem5 v25.

Cada request de memoria debe conservar:

```text
command_id
register_ref
destination_byte_range
element_index
lane_id
address
is_load / is_store
fault_and_order_metadata
```

`register_ref` identifica el grupo de registros: `VectorRegRef`
arquitectónico en el baseline o `PhysicalRegRef`, con su versión, cuando se
habilita el renombramiento. El modo sin renombramiento no exige
`physical_version`. La petición conserva además su rango de bytes y su
identidad de comando y subpetición (`CommandKey` y `requestId` en las
interfaces propuestas).

Al recibir una respuesta, la VLSU la distribuye a la lane propietaria y conserva
la identidad de la operación, del elemento y la referencia original del
registro, incluida su versión sólo en el modo con renombramiento. La política
exacta para hacer visible la disponibilidad del dato y habilitar chaining se
definirá en una fase posterior.

En el baseline FIFO, los stores se mantienen en orden. Los loads pueden tener
varias requests en vuelo, pero cada respuesta debe conservar su identidad para
no publicar bytes en un grupo de registros o lane incorrectos ni, con
renombramiento, en una versión física distinta.

| Archivo | Acción | Vitruvius necesario |
|---|---|---|
| `gem5_VPU/src/cpu/vector_backend/memory/ara_vlsu.hh/.cc` | 🟦 Crear: VLSU con requests identificadas y distribución por lane. | No existe; debe crearse. |
| `gem5_VPU/src/cpu/vector_backend/memory/vector_memory_backend.hh` | 🟦 Crear: frontera entre VLSU y memoria gem5. | No existe; debe crearse. |
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
| `gem5_VPU/src/cpu/vector_backend/frontend/readiness_table.hh/.cc` | 🟦 Crear: readiness por bytes para versiones físicas y por bits para máscaras. | No existe; debe crearse. |
| `gem5_VPU/src/cpu/vector_backend/lanes/task_distributor.hh/.cc` y `ara_lane.hh/.cc` | 🟨 Modificar después: consultas y despertadores de disponibilidad granular. | No en las primeras fases. |
| `gem5_Vitruvius/src/cpu/vector_engine/vpu/register_file/vector_reg_valid_bit.hh/.cc` | 🟩 Rescatar sólo para entender el límite del valid bit por registro. | No reutilizar directamente: su granularidad es insuficiente. |

## 10. Orden de implementación

1. Añadir el flag `vectorOffloadEnabled` a MinorCPU.
2. Evitar que Minor descomponga RVV en microoperaciones cuando el offload esté activo.
3. Crear `VectorCommand`.
4. Implementar `requestGrant`, `dispatch`, `accepted` y `completed`.
5. Crear `AraSequencer` y una única FIFO de comandos.
6. Implementar un backend funcional simple sin renombramiento ni chaining.
7. Añadir lanes físicas y distribución por palabras.
8. Añadir VRF distribuido y bancarizado.
9. Añadir VLSU, SLDU, MASKU e interconexión.
10. Añadir el modo opcional `enable_vector_renaming`.
11. Añadir la tabla de readiness por bytes como preparación para el chaining.
12. Implementar chaining granular después de validar el modelo de lanes, VRF y
    colas de operandos.
13. Evaluar bypass, OoO o políticas de issue alternativas sólo después de
    validar el baseline FIFO.

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
