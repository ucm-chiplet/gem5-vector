# Hito 2: pendientes de MinorCPU en las fases 1--5

Informe del 4 de noviembre de 2026 sobre `f0566a6ae4` (rama `feature/vpu`).
Recoge lo que falta en la parte CPU/Minor de las fases 2.1--2.5 para que
las entregas VPU registradas sean utilizables desde un programa. El 5 de
noviembre de 2026 se añade la [sección 2.6](#fase-26-operaciones-enteras-y-fp32)
con los pendientes CPU de la [entrega VPU de fase 6][fase6], y la
[sección 2.7](#fase-27-slides) con los de la [entrega VPU de fase 7][fase7].
No es una
propuesta de código ni modifica el alcance de ninguna fase: cada cambio
seguirá requiriendo su propuesta y aprobación.

**Revisiones posteriores.** El 8 de noviembre de 2026 el autor de la rama
de Minor marcó como resueltas las acciones que implementó (commits
`7b3456c198` a `963e6a9550`). El 11 de noviembre de 2026, tras integrar
esa rama en `feature/vpu` por fast-forward, se contrastaron esas
acciones con el código en `2c3b45792f` y se actualizaron el resumen, el
recorrido actual y la evidencia común. "Resuelto" significa implementado
y verificado leyendo el código; ninguna acción tiene todavía evidencia de
ejecución desde un programa. El trabajo restante se organiza en el
[plan de cierre de la fase 8][plan-f8].

La parte CPU forma parte del cierre conjunto de cada fase. Su ausencia
limita O2 (recursos finitos observables desde cargas reales) y O6
(capacidades y evidencia trazables): sin ella, las capacidades backend de
las fases 2--5 no producen comportamiento observable en un programa.

## Método y fuentes

Se han contrastado:

- La [guía de responsabilidades][responsabilidades], acciones CPU 2.1--2.5.
- Las entregas de las [fases 1][fase1], [2][fase2], [3][fase3], [4][fase4]
  y [5][fase5], y las secciones 7, 13.1--13.5 y 15.3 del [diseño][diseno].
- El código de `src/cpu/minor/` (`decode.cc`, `execute.hh/.cc`,
  `vector_types.hh`, `vector_state.hh`, `scoreboard.cc`, `cpu.cc`,
  `BaseMinorCPU.py`, `stats.hh/.cc`), la frontera
  `src/cpu/vector_engine/interface/` y `configs/example/riscv_vpu.py`.

El último cambio en `src/cpu/minor/` es `c228fc9c62` (26 de octubre de
2026), dentro de la fase 1. Las entregas de las fases 2--5 declaran
expresamente que no modifican Minor. Las referencias a líneas corresponden
al commit indicado.

Se distingue en cada punto entre **ausente** (no hay código), **parcial**
(existe una parte o sólo funciona por el despacho serie actual) y **sin
evidencia** (el código existe, pero no consta ejecución registrada). Este
informe es revisión de fuentes: no se ha compilado ni ejecutado nada.

## Resumen

| Fase | Entrega VPU | Parte CPU/Minor | Efecto desde un programa |
| --- | --- | --- | --- |
| 2.1 Contratos | Integrada | Integrada; resueltos rechazos (rejectionReason), estados terminales y matriz de soporte. | Sin cambios respecto al hito 1. |
| 2.2 Anchos y grupos | Backend 8/16/32/64, m1--m8, mf2--mf8 | Decode implementado para memoria en todos los EEW (8/16/32/64) y suma entera | Memoria y suma funcional en todos los anchos; la configuración de ejemplo tiene SEW 16/32 y m1 por defecto. |
| 2.3 Recursos | Recursos temporizados | Métricas de espera (`vectorWaitGrantCycles`, `vectorWaitCompletionCycles`) y diseño de 2.5; sin evidencia de esperas variables ni de drain con obligaciones | Las esperas tienen causa observable en Minor. |
| 2.4 Bloques | Bloques unit-stride | Barrera antes de memoria vectorial revisada; faults por bloque sin evidencia | Igual que el baseline; sin evidencia de faults por bloque. |
| 2.5 Concurrencia | Varios comandos activos | Ventana de despacho desacoplada de hasta 2 comandos en vuelo activa | Dos comandos aritméticos independientes pueden solaparse en la VPU. |
| 2.6 Enteras y FP32 | Mul/Div/resto, FP32, varias ALUs | Decode `.vv/.vx` y FP32 `.vv`, `mstatus.FS/VS`, `Dirty` al retirar | Las operaciones son alcanzables desde un programa. |
| 2.7 Slides | Crossbar y `SlideUnit` | Decode, `SlideCommand` y normalización del escalar | Las slides y la red son alcanzables desde un programa. |
| 2.8 Pathfinder | `vmin`/`vminu` `.vv` en ALU | Decode de `vmin.vv` (con signo, cualquier SEW); `vminu.vv` sin decode | Los 7 tipos RVV de Pathfinder son alcanzables; Pathfinder sin ejecutar. |

La consecuencia principal es que los criterios de cierre de la
[sección 15.3][cierre] que dependen de un programa ya tienen camino
CPU--VPU implementado, pero ninguno puede acreditarse todavía: no hay
ejecución registrada tras estos cambios. Esa evidencia se organiza en el
[plan de cierre de la fase 8][plan-f8].

## Lo que Minor ya hace

Recorrido en `2c3b45792f`. Las referencias se dan por función, porque
las líneas cambian con cada revisión:

1. **Decode** (`decode.cc`) clasifica cada instrucción en `CpuScalar`,
   `CpuVectorConfig`, `VpuOffload`, `NativeVector` o `UnsupportedVector`.
   `tryDecodeMemoryOp` reconoce `vle/vse` unit-stride con EEW 8/16/32/64;
   `tryDecodeArithmeticOp` reconoce `vadd`, `vmul`, `vdiv(u)` y `vrem(u)`
   `.vv/.vx`, `vfadd/vfmul/vfdiv.vv` y `vmin.vv`; `classifyVectorSlideOp`
   reconoce `vslide1up/down.vx`. Las no soportadas reciben
   `IllegalInstFault` en decode. Las `VpuOffload` no se descomponen en
   microoperaciones.
2. **Issue** comprueba sólo las fuentes escalares en el scoreboard de
   Minor, marca los destinos como impredecibles hasta la retirada y crea
   un `MinorVectorState` por instrucción en `vpuOffloadStates`.
3. **Despacho** (`advanceVectorDispatch`) recorre las primeras
   `maxActiveVectorOffloads` entradas (2 por defecto), con hasta
   `vectorDispatchWidth` envíos por ciclo (1 por defecto). Comprueba
   `misa`/`mstatus.VS` y, en FP, `mstatus.FS` antes de reservar; aplica
   las barreras de su [diseño de concurrencia][concurrencia-minor]
   (consecutividad, memoria sólo en cabeza, sin rechazo previo); asigna
   `CommandKey`, captura configuración y operandos una vez, reintenta tras
   `Stall` y guarda `rejectionReason` ante `Rejected`.
4. **Callbacks** `accepted()` y `completed()` localizan la entrada por
   `CommandKey` en toda la ventana.
5. **Retirada** (`commitInst`) procesa sólo la cabeza y en orden: traduce
   el motivo de rechazo en `IllegalInstFault`, aplica `fflags`,
   `finalVstart` y el fault de memoria, marca `mstatus.VS` (y `FS`) como
   `Dirty`. `IllegalInstruction` se convierte en `IllegalInstFault`,
   `Cancelled` retira sin efectos e `InternalError` termina en `panic`.
6. **Orden con memoria escalar**: la memoria vectorial espera a
   `lsq.storeBuffer.isDrained()` y `hasPendingVpuStoreBefore()` retiene
   los accesos escalares posteriores, también en la emisión anticipada.
7. **Descarte y drain**: `mustCompleteVpuOffload()` e `isDrained()`
   inspeccionan todas las entradas de `vpuOffloadStates`.
8. **Estadísticas** de espera, ocupación de la ventana y causas de bloqueo
   en `MinorStats`.

Las tablas siguientes conservan, por fase, el estado de cada acción de la
guía.

## Fase 2.1: contratos y capacidades

La [entrega de fase 1][fase1] integró la captura única, el
`MinorVectorState`, la distinción `Stall`/`Rejected`/`Granted` y el
contexto FP. Quedan estos huecos:

| Pendiente | Estado | Detalle |
| --- | --- | --- |
| Uso de `rejectionReason` | Resuelto | `GrantResult.rejectionReason` se conserva en `MinorVectorState` ante `Rejected`. En `commitInst`, Minor emite traza `DPRINTF(MinorExecute)` con el motivo específico (`UnsupportedConfiguration`, `InvalidRegisterGroup`, etc.) y lo incluye en el diagnóstico del `IllegalInstFault`. |
| Estados terminales distintos de `Success` y `MemoryFault` | Resuelto | `commitInst` gestiona explícitamente todos los estados de `CompletionStatus`: `IllegalInstruction` invoca `IllegalInstFault` normativo de RISC-V, `Cancelled` descarta la instrucción limpiamente sin alterar estado arquitectónico ni inflar el commit, e `InternalError` emite `panic` descriptivo con contexto e identidad del comando. |
| Separar decode, representación y soporte | Resuelto | Formalizado en [`Matriz de decode, representacion y soporte.md`](Matriz%20de%20decode,%20representacion%20y%20soporte.md). Se documenta por separado lo que decodifica Minor, lo que transporta `VectorCommand` y lo que valida `VectorSupport`, aclarando la distinción entre limitación de perfil/modelo (`EEW == SEW`) y la norma RVV 1.0. |

**Observación sobre RVV.** Un rechazo por perfil (por ejemplo, `vle32.v`
con SEW=16, legal en RVV con EEW≠SEW) llega al programa como instrucción
ilegal. Es coherente con la exclusión acordada de EEW≠SEW, pero conviene
que la matriz de soporte lo exprese como limitación del modelo y no como
ilegalidad RVV ([manual RVV 1.0, edición 20240411][rvv], *Vector Load/Store
Width Encoding*).

## Fase 2.2: anchos y grupos

Es el pendiente con más efecto funcional, porque la VPU ya admite la
matriz completa en backend ([entrega de fase 2][fase2]).

### Decode de memoria y anchos en MinorCPU

En `src/cpu/minor/decode.cc`, `Decode::decodeVectorOp()` delega en
`tryDecodeMemoryOp()`, que reconoce genéricamente las macroinstrucciones
`RiscvISA::VleMacroInst` y `RiscvISA::VseMacroInst`. El ancho efectivo
de elemento (EEW) se extrae directamente de la codificación de la instrucción
vía `RiscvISA::width_EEW(rv_inst->machInst.width)`, cubriendo 8, 16, 32 y 64
bits sin depender del estado dinámico de SEW en la CPU.

| Instrucción del perfil | Estado CPU | Consecuencia |
| --- | --- | --- |
| `vadd.vv`, `vadd.vx` en SEW 8/16/32/64 | Decodificada; SEW y LMUL se toman de `vtype` | Representable en todos los anchos. Sin evidencia ejecutada fuera de SEW=32, m1. |
| `vle8.v`, `vle16.v`, `vle64.v` | Resuelto | Decodificada dinámicamente con EEW={8,16,64} en `tryDecodeMemoryOp`. |
| `vse8.v`, `vse16.v`, `vse64.v` | Resuelto | Decodificada dinámicamente con EEW={8,16,64} en `tryDecodeMemoryOp`. |
| `vle32.v`, `vse32.v` con SEW≠32 | Decodificada con EEW=32 | Decodificada correctamente; la VPU comprueba EEW=SEW y rechaza con `UnsupportedConfiguration` si no coinciden. |

### Grupos y configuración

- **Representación de grupos: presente.** `buildVectorCommand()` obtiene
  el número de registros con `vtype_regs_per_group()`, que devuelve uno
  para LMUL fraccional (`execute.cc:142-152`; `utility.hh:276-280`), y
  `captureVectorConfig()` traduce `vlmul` con `decodeVlmul()`, incluidos
  `mf2/mf4/mf8` (`execute.cc:79-126`). No consta ejecución con LMUL≠1.
- **Normalización `.vx`: presente.** Se aplica una vez con
  `ElementCodec::normalizeScalar()` según XLEN y SEW
  (`execute.cc:187-200`). No consta ejecución con SEW 8/64 ni con
  escalares negativos.
- **Configuración de ejemplo: limitada.** `configs/example/riscv_vpu.py`
  usa `VpuConfiguration` con sus valores por defecto, `supported_sews =
  (16, 32)` y `supported_lmuls = (0,)` (`configuration.py:176-177`). Sin
  modificar el script, SEW 8/64 y cualquier LMUL distinto de m1 se rechazan
  y llegan al programa como instrucción ilegal. Falta exponer el perfil y
  comprobar que la palabra de lane admite SEW=64 (al menos 8 bytes).

### Evidencia

El [benchmark del baseline](../../benchmarks/baseline/README.md) usa sólo
SEW=32 y m1. No hay casos de programa para los cuatro anchos, cruces de
grupos `m4/m8`, extremos de `v0`--`v31`, tail fraccional ni cambios de SEW.
La [entrega de fase 2][fase2] recoge estos recorridos como sugerencias del
equipo, sin implementar.

## Fase 2.3: esperas, admisión y finalización

Gran parte del protocolo serie ya existe: retención del comando ante
`Stall` con reactivación de Execute (`execute.cc:2123-2132`), consumo
único del token, preparación del estado antes de `dispatch()`, liberación
del descriptor en `accepted()`, retirada desde commit y `panic` ante
respuestas desconocidas o en fase incorrecta.

| Pendiente | Estado | Detalle |
| --- | --- | --- |
| Preparar la propuesta CPU de 2.5 (acción 7) | Resuelto | Documentado en [`Diseno de concurrencia MinorCPU - Fase 2.5.md`](Diseno%20de%20concurrencia%20MinorCPU%20-%20Fase%202.5.md). Se formaliza la separación de cursor de despacho (`advanceVectorDispatch`) y retiro (`commitInst`), el seguimiento por `CommandKey`, las barreras de apertura y la capacidad configurable (`maxActiveVectorOffloads`). |
| Observabilidad de esperas CPU | Resuelto | Integradas métricas escalares en `MinorStats` (`stats.hh/.cc` y `execute.cc`): `vectorWaitGrantCycles` (espera de admisión/contrapresión), `vectorWaitCompletionCycles` (espera de ejecución VPU en `Dispatched/Accepted`), `vectorRetainedCompletionCycles` (instrucciones jóvenes retenidas en `Completed` esperando retiro) y `vectorOffloadCommands` (total de comandos despachados). |
| Esperas variables | Sin evidencia | Las fases 3--5 alargan y hacen variables las esperas (recursos temporizados, bloques y valores por defecto con cuatro entradas). Ninguna entrega registra una ejecución de programa posterior a esos cambios. |
| Drain con obligaciones | Sin evidencia | El código protege las entradas con obligaciones y `DrainAllInsts` descarta las que aún no tienen reserva. No consta un recorrido de drain con un comando en `WaitGrant`, `Dispatched` o `Accepted`. |

## Fase 2.4: orden con memoria escalar y faults

Las reglas de orden y el tratamiento de faults proceden del hito 1
(`19c0f50ef1`) y la [fase 4][fase4] no modifica Minor. Las acciones CPU de
esta fase no se han revisado de nuevo con la memoria por bloques.

| Pendiente | Estado | Detalle |
| --- | --- | --- |
| Barrera antes de memoria vectorial (acciones 1--2) | Resuelto | Revisión explícita completada: el despacho en cabeza (`i == 0`) y la espera a `storeBuffer.isDrained()` garantizan el drenado de stores y fences (`BarrierDataRequest`) anteriores. Las operaciones posteriores (incluyendo AMOs y LR/SC, clasificadas como `isMemRef()` vía `IsAtomic`/`IsLLSC`) se retienen estrictamente mediante `hasPendingVpuStoreBefore()` en emisión normal y anticipada. |
| Fault por bloque (acciones 5--6) | Sin evidencia | Minor aplica el `finalVstart` recibido e invoca el `Fault` (`execute.cc:1355-1371`). Con bloques, ese índice es el primer elemento del bloque que falla. No consta ejecución de faults de traducción o acceso, y el tratamiento en SE de esos faults RISC-V no está documentado. |
| Precisión del trap | Pendiente compartido | La [fase 4][fase4] deja pendiente contrastar en RVV 1.0 que `firstElement` cumple la precisión del trap en stores y la regla de elementos desalineados. Minor no puede corregir un índice; sólo lo aplica. |
| Visibilidad escalar/vectorial | Pendiente de configuración | En el ejemplo, la VPU y la CPU comparten `SystemXBar` sin cachés. No hay topología acordada con cachés CPU que defina la visibilidad de los stores vectoriales (validación «Memoria escalar y vectorial» de la [sección 15.2][recorridos]). |

## Fase 2.5: despacho y retirada de varias instrucciones

Se ha implementado el desacoplo de despacho y retiro en MinorCPU para admitir
una ventana de concurrencia acotada de hasta 2 instrucciones simultáneas en
vuelo hacia la VPU.

| Acción de la guía | Estado | Situación en el código |
| --- | --- | --- |
| 1. Separar cursor de despacho y cabeza de retirada | Resuelto | `advanceVectorDispatch()` recorre los estados activos independientemente del ciclo de commit de la cabeza, permitiendo despachar una segunda instrucción tras otorgarse grant a la primera. |
| 2. Ventana acotada y punto seguro de apertura | Resuelto | Acotada con `MaxActiveVectorOffloads = 2` y rate-limiting de como máximo un despacho por ciclo (`dispatched_this_cycle`). Se invoca desde `commitInst` cuando la cabeza de `inFlightInsts` es vectorial, asegurando orden de control. |
| 3. Una entrada por instrucción, con captura única | Resuelto | `MinorVectorState` se crea en issue y retiene el comando inmutablemente; ante reintentos por contrapresión (`Stall`) no se recaptura ni se regenera la clave. |
| 4. Callbacks por clave | Resuelto | `accepted()` y `completed()` buscan la instrucción correspondiente mediante `CommandKey` en todo `vpuOffloadStates`, sin asumir que la respuesta corresponde a `front()`. |
| 5. Retener finalizaciones jóvenes | Resuelto | Las finalizaciones de instrucciones jóvenes se almacenan en su propio descriptor (`completion`), y sus efectos arquitectónicos y retiro se aplican estrictamente en orden cuando llegan a la cabeza de `inFlightInsts`. |
| 6. Cierre de ventana ante memoria, escalares, configuración, CSRs, control, rechazo y `vstart≠0` | Resuelto | Se imponen barreras estrictas: sólo se admite concurrencia si la instrucción joven es estrictamente consecutiva (`execSeqNum == prev + 1`), puramente aritmética (`DecodedArithmeticOp`), con `vstart == 0` y sin fallos previos. |
| 7. Activación, descarte, interrupciones y drain para todas las entradas | Resuelto | `mustCompleteVpuOffload()` y los chequeos de vaciado (`isDrained()`) inspeccionan todas las entradas activas de `vpuOffloadStates`. |
| 8. Estadísticas de Minor | Resuelto | Integradas en `MinorStats` (`stats.hh/.cc` y `execute.cc`): ocupación y distribución de comandos activos en vuelo (`vectorActiveCommandsDistribution`, `vectorMultiActiveCycles`) y causas de bloqueo de la ventana concurrente (`vectorBlockedCapacity`, `vectorBlockedOrder`, `vectorBlockedStructural`, `vectorBlockedVstart`). |

Puntos de diseño que la propuesta CPU de 2.5 deberá cerrar:

- **Dependencias.** El scoreboard de Minor ignora las fuentes vectoriales
  de las instrucciones `VpuOffload` y delega RAW/WAR/WAW en el scoreboard
  VPU. Debe confirmarse que esto sigue siendo seguro con ventana y que las
  fuentes escalares de `.vx` siguen esperando a su productor.
- **Rechazo de una instrucción joven.** Si la VPU rechaza una instrucción
  dentro de la ventana, las anteriores deben terminar antes de procesar su
  excepción, y no debe despacharse ninguna posterior (sección 7 del
  [diseño][diseno]).
- **Liberación anticipada en VPU.** La VPU libera el scoreboard al
  finalizar, sin esperar a la retirada, porque una aritmética aceptada no
  puede fallar en el hito 2. Minor debe mantener esa garantía: no admitir
  en la ventana operaciones que puedan fallar después de aceptarse.
- **Interrupciones y squash.** Ningún trabajo despachado puede anularse
  sin contrato de recuperación. Debe precisarse cuándo se toma una
  interrupción con varias entradas despachadas (en SE no hay controlador
  de interrupciones, pero el camino FS sigue presente en el código).

Estado el 11 de noviembre de 2026: el [diseño de
concurrencia][concurrencia-minor] trata el rechazo de una instrucción
joven (no se despacha nada tras un rechazo) y menciona el squash. No
documenta expresamente la seguridad de delegar dependencias vectoriales
en el scoreboard VPU con ventana, la liberación anticipada ni la toma de
interrupciones con varias entradas despachadas. Su revisión es parte del
paquete P5 del [plan de cierre de la fase 8][plan-f8].

Sin estos puntos no puede cerrarse el criterio de la
[sección 13.5][fase5-diseno] («Minor debe despachar más de una
instrucción») ni el criterio 1 de la [sección 15.3][cierre].

## Fase 2.6: operaciones enteras y FP32

La [entrega VPU de fase 6][fase6] admite en backend `vmul`, `vdiv`,
`vdivu`, `vrem` y `vremu` (`.vv`/`.vx`, 8/16/32/64 bits) y `vfadd.vv`,
`vfmul.vv` y `vfdiv.vv` FP32, y no modifica Minor. Las acciones de la
[guía 2.6][responsabilidades] quedan así:

| Pendiente | Estado | Detalle |
| --- | --- | --- |
| 1. Decode de las operaciones nuevas | Resuelto | `classifyVectorArithOp` en `decode.cc` reconoce desacopladamente `vadd_vv/vx`, `vmul_vv/vx`, `vdiv_vv/vx`, `vdivu_vv/vx`, `vrem_vv/vx`, `vremu_vv/vx`, `vfadd_vv`, `vfmul_vv` y `vfdiv_vv`, propagando `ArithmeticOperation` y `signedness`. |
| 2. Orden de operandos | Resuelto | `tryDecodeArithmeticOp` asigna `vectorSource = vs2` (srcRegIdx(1)) y `secondOperand = vs1/rs1` (srcRegIdx(0)), cumpliendo la semántica RVV 1.0 (`vd = vs2 op vs1`) para operaciones no conmutativas (`vdiv`, `vrem`, `vfdiv`). |
| 3. Comprobación de `mstatus.FS` y `VS` | Resuelto | Se comprueba en `advanceVectorDispatch` (`execute.cc`) antes de emitir reserva: si `!misa.rvv` o `status.vs == Off`, se genera excepción ilegal `"RVV is disabled or VPU is off"`; si la operación requiere FPU (`requiresFpContext`) y `status.fs == Off`, se genera `"FPU is off"`. Se añade `cpuFaultMessage` a `MinorVectorState` y el contador `vectorRejectedCommands` en `MinorStats`. |
| 4. Estado `Dirty` al retirar | Resuelto | En `commitInst` ante `CompletionStatus::Success`, se invocan las funciones nativas de gem5 `RiscvISA::updateVPUStatus` (con `set_dirty=true`) para marcar `mstatus.vs = Dirty` y `RiscvISA::updateFPUStatus` para marcar `mstatus.fs = Dirty` si la instrucción produjo flags de coma flotante o es de tipo FP. |
| 5. Captura del redondeo | Presente, sin evidencia | `buildVectorCommand()` captura `frm` para toda ficha con `requiresFpContext` y conserva `Invalid` si `frm` es 5--7; admisión lo rechaza y Minor lo convierte en instrucción ilegal. No consta ejecución. |
| 6. Flags al retirar | Resuelto, sin evidencia | Cada entrada conserva sus `fpFlags` en su propia `completion` y se acumulan en `fflags` cuando esa entrada retira como cabeza, también con la ventana de 2.5 (revisión del 11 de noviembre de 2026). |
| 7. CSRs FP como límite de ventana | Resuelto | Garantizado por la barrera de consecutividad estricta en `advanceVectorDispatch` (`execSeqNum == prev + 1`). Cualquier acceso escalar a `frm`, `fflags` o `fcsr` entre instrucciones vectoriales rompe la consecutividad e interrumpe de inmediato el despacho concurrente hasta que las instrucciones previas completen y se retiren en orden. |

Sin los puntos 1--3 no puede acreditarse el criterio 4 (recorrido
CPU--FPU--VRF--CPU) ni el 13 (Mul/Div/resto mediante la ALU) de la
[sección 15.3][cierre]. Observar el progreso independiente de ALU y FPU con
instrucciones distintas requiere además la parte CPU de 2.5.

## Fase 2.7: slides

La [entrega VPU de fase 7][fase7] admite en backend `vslide1up.vx` y
`vslide1down.vx` (`vm=1`, 8/16/32/64 bits) mediante un `SlideCommand`
propio y no modifica Minor. Las acciones de la [guía 2.7][responsabilidades]
quedan así:

| Pendiente | Estado | Detalle |
| --- | --- | --- |
| 1. Decode | Resuelto | `decodeVectorOp()` clasifica `vslide1up_vx` y `vslide1down_vx` mediante `classifyVectorSlideOp()` en `decode.cc`, empaquetándolas en `DecodedSlideOp` como `VectorExecutionClass::VpuOffload` sin descomponer la macro-op en micro-ops nativas y extrayendo `vd`, `vs2` y `rs1`. |
| 2. Construcción del comando | Resuelto | `buildVectorCommand()` en `execute.cc` construye `SlideCommand` cuando el payload es `DecodedSlideOp`, empaquetando `operation`, `destination` (`vd`), `vectorSource` (`vs2`) con sus grupos LMUL, y el valor del escalar `rs1` leído de la CPU. |
| 3. Normalización del escalar | Resuelto | Integrada en `buildVectorCommand()` (`execute.cc`): se invoca `ElementCodec::normalizeScalar(slide.scalar, xlen, sew)` una sola vez en CPU antes del envío, extendiendo signo si XLEN < SEW o conservando bits bajos si XLEN > SEW. Se añade además la estadística `vectorSlideCommands` en `MinorStats`. |
| 4. Solapamiento en up | Resuelto | Admisión en VPU rechaza `vslide1up` con `vd == vs2` (`InvalidRegisterGroup`). La infraestructura de rechazos de Minor (`execute.cc`) lo convierte en `IllegalInstFault` antes de aplicar efectos arquitectónicos en retiro. |
| 5. Formas con máscara | Resuelto | `decodeVectorOp()` captura `masked = !vm` (bit 25). Al despacharse `vm=0`, admisión en VPU rechaza por `UnsupportedConfiguration` y Minor lo traduce a `IllegalInstFault` en retiro. |
| 6. Protocolo y retirada | Sin cambios | Se usa el mismo protocolo de reserva, aceptación, finalización y retirada serie. Minor no crea paquetes de red; espera la finalización del comando. No tiene `fpContext` ni produce flags. |
| 7. `mstatus.VS` | Resuelto | Comprobado genéricamente en `advanceVectorDispatch` (`execute.cc`): si `!misa.rvv` o `status.vs == Off`, la instrucción vectorial se aborta con excepción de instrucción ilegal antes de solicitar reserva. |

Sin los puntos 1--3 no puede acreditarse el criterio 6 (slides desde
programa, con la red en los casos que cruzan lanes) de la
[sección 15.3][cierre]. El criterio 14 depende sólo de la VPU, pero su
tráfico no se observa en un programa hasta resolver esos puntos.

## Evidencia pendiente común a las fases 1--8

Estado el 11 de noviembre de 2026:

- **Ejecución de programas.** Pendiente. Las entregas VPU 2--8 registran
  compilación y, algunas, `--help`. No hay registro de ejecución de una
  carga con Minor tras esos cambios ni tras la integración de la rama de
  Minor, ni siquiera del baseline del hito 1 con los valores por defecto
  actuales (cuatro entradas de cola, cuatro activos, cuatro contextos por
  lane y ventana de Minor de 2).
- **Compilación de Minor.** Resuelto: `build/RISCV/gem5.opt` compila y
  enlaza en `2c3b45792f`, con Minor y todas las cabeceras VPU actuales.
- **Matriz CPU--VPU.** Existe la [matriz de decode, representación y
  soporte][matriz]. Su consolidación con la tabla de la fase 6 y la
  distinción entre implementado y comprobado es el paquete P6 del
  [plan de cierre de la fase 8][plan-f8].

## Fuera del alcance de este informe

No son pendientes de las fases 1--5, aunque aparezcan como `-TODO` en el
código:

- Destinos escalares (`vector_types.hh`, comentario en
  `DecodedArithmeticOp::destination`): hito 3.
- Operandos inmediatos (`.vi`): no forman parte de la matriz del hito 2.
- Cierre de Pathfinder: fase 2.8, en el [plan de cierre][plan-f8].
- Decode de `vminu.vv`: no lo exige Pathfinder; la VPU ya lo admite.
- Predicación (`vm=0`), chaining y memoria concurrente: hito 3.

Las fases 2.6 y 2.7, antes listadas aquí, tienen ya sus secciones y su
parte CPU implementada.

## Orden sugerido

Superado el 11 de noviembre de 2026: la rama de Minor implementa los
pasos 1--3 y revisa la barrera del 4; los faults por bloque siguen sin
evidencia. Se conserva como registro; el trabajo restante sigue
el [plan de cierre de la fase 8][plan-f8].

Era una sugerencia para preparar propuestas, no un plan aprobado:

1. **2.2 CPU**: decode de `vle8/16/64.v` y `vse8/16/64.v` con EEW desde la
   codificación, perfil expuesto en `riscv_vpu.py` y matriz de soporte.
   Desbloquea el criterio 3 de cierre.
2. **2.1/2.3 CPU**: uso de `rejectionReason`, decisión sobre los estados
   terminales restantes y primeras estadísticas de espera en Minor. Son
   cambios pequeños y facilitan diagnosticar 2.5.
3. **2.5 CPU**: propuesta de ventana aritmética según la sección 7 del
   [diseño][diseno], incluidos los puntos de diseño anteriores.
4. **2.4 CPU**: revisión de barreras y faults con bloques, en paralelo o
   tras 2.5, ya que la ventana no atraviesa memoria.

Comprobaciones sugeridas al equipo, sin implementar: el baseline con los
valores por defecto actuales; suma, carga y store en los cuatro anchos y
en `m4`/`mf2`; un `vle32.v` con SEW=16 para confirmar el rechazo previo a
efectos; un fault de traducción entre bloques; y, tras 2.5, dos `vadd`
independientes con `sequencer.overlappedStarts > 0`.

[responsabilidades]: <Responsabilidades CPU y VPU.md>
[diseno]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md>
[cierre]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[recorridos]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#152-recorridos-sugeridos-al-equipo>
[fases]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#13-fases-de-trabajo-y-dependencias>
[fase5-diseno]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#135-fase-5-concurrencia-desde-minor-hasta-las-lanes>
[fase1]: <Fase 1 - Contratos y capacidades.md>
[fase2]: <Fase 2 - Anchos y grupos.md>
[fase3]: <Fase 3 - Recursos temporizados y arbitraje.md>
[fase4]: <Fase 4 - Memoria unit-stride por bloques.md>
[fase5]: <Fase 5 - Concurrencia en la VPU.md>
[fase6]: <Fase 6 - FPU, operaciones enteras y varias ALUs.md>
[fase7]: <Fase 7 - Interconexión y slides.md>
[plan-f8]: <Plan de cierre de la fase 8.md>
[matriz]: <Matriz de decode, representacion y soporte.md>
[concurrencia-minor]: <Diseno de concurrencia MinorCPU - Fase 2.5.md>
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
