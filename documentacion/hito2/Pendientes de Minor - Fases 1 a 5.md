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
| 2.3 Recursos | Recursos temporizados | Protocolo serie presente; sin métricas ni preparación de 2.5 | Esperas más largas sin causa observable en Minor. |
| 2.4 Bloques | Bloques unit-stride | Barreras y faults del hito 1; sin revisión con bloques | Igual que el baseline; sin evidencia de faults por bloque. |
| 2.5 Concurrencia | Varios comandos activos | Ventana de despacho desacoplada de hasta 2 comandos en vuelo activa | Dos comandos aritméticos independientes pueden solaparse en la VPU. |

La consecuencia principal es que ningún criterio de cierre de la
[sección 15.3][cierre] que dependa de un programa puede acreditarse todavía:
ni el 1 (dos instrucciones del programa simultáneas) ni el 3 (suma y
memoria en los cuatro anchos y en el perfil LMUL).

## Lo que Minor ya hace

Para delimitar los pendientes, éste es el recorrido actual:

1. **Decode** (`decode.cc:58-133`) clasifica cada instrucción en
   `CpuScalar`, `CpuVectorConfig`, `VpuOffload`, `NativeVector` o
   `UnsupportedVector`. Las no soportadas reciben `IllegalInstFault` en
   decode (`decode.cc:251-262`). Las `VpuOffload` no se descomponen en
   microoperaciones.
2. **Issue** (`execute.cc:825-862`) comprueba sólo las fuentes escalares en
   el scoreboard (`scoreboard.cc:282-320`), marca los destinos como
   impredecibles hasta la retirada y crea un `MinorVectorState` por
   instrucción en `vpuOffloadStates`.
3. **Commit** (`execute.cc:1254-1385`) recorre las fases `ReadyToGrant`,
   `WaitGrant`, `Dispatched`, `Accepted` y `Completed` **sólo para la
   cabeza de retirada**. Asigna `CommandKey`, captura configuración y
   operandos una vez, reintenta tras `Stall`, consume el token una vez y
   despacha. En `Completed` acumula `fflags`, aplica `finalVstart` e
   invoca el fault si lo hay.
4. **Callbacks** (`execute.cc:1180-1221`) localizan la entrada mediante
   `vpuOffloadStates.front()` y fallan con `panic` si la clave no coincide.
5. **Orden con memoria escalar**: la memoria vectorial espera a
   `lsq.storeBuffer.isDrained()` (`execute.cc:1286-1288`) y
   `hasPendingVpuStoreBefore()` retiene los accesos escalares posteriores,
   también en la emisión anticipada (`execute.cc:1387`, `1671`).
6. **Descarte y drain**: una entrada con reserva u obligaciones no se
   descarta (`execute.cc:1732-1744`); `isDrained()` exige
   `vpuOffloadStates` vacío (`execute.cc:2429-2445`).

Este recorrido cumple el protocolo de una instrucción activa. Los
pendientes siguientes son lo que falta respecto a la guía de cada fase.

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
| Barrera antes de memoria vectorial (acciones 1--2) | Parcial | Sólo se consulta `storeBuffer.isDrained()` (`execute.cc:1286-1288`). Las instrucciones escalares anteriores ya se han retirado porque el despacho ocurre en la cabeza, y los accesos posteriores se retienen con `hasPendingVpuStoreBefore()`. La corrección depende del despacho serie: falta una revisión explícita frente a LSQ (`accessesInFlight()`), AMO, LR/SC, `fence` y accesos no cacheables. |
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
| 1. Decode de las operaciones nuevas | Ausente | `decodeVectorOp()` sólo reconoce `vadd_vv/vx`, `vle32_v` y `vse32_v` (`decode.cc:76-84`). Faltan `vmul_vv/vx`, `vdiv_vv/vx`, `vdivu_vv/vx`, `vrem_vv/vx`, `vremu_vv/vx`, `vfadd_vv`, `vfmul_vv` y `vfdiv_vv`, con su `ArithmeticOperation` y `signedness` (`Signed` para `vdiv`/`vrem`, `Unsigned` para `vdivu`/`vremu`, `NotApplicable` en el resto). |
| 2. Orden de operandos | Incorrecto para operaciones no conmutativas | En gem5, `srcRegIdx(0)` es `vs1`/`rs1` y `srcRegIdx(1)` es `vs2` (`formats/vector_arith.isa`). Para `.vv`, el decode coloca `vs1` en `vectorSource` (`decode.cc:115-122`). El contrato de fase 6 exige `vectorSource = vs2` y `secondOperand = vs1/rs1`, porque RVV define `vd = vs2 op vs1`. Es inocuo en `vadd`, pero invertiría `vdiv`, `vrem` y `vfdiv`. Conviene leer `vs1`/`vs2` de los campos de `machInst` en lugar del orden de `srcRegIdx`. |
| 3. Comprobación de `mstatus.FS` y `VS` | Ausente | La ruta de offload no ejecuta la `StaticInst`, de modo que no pasa por `updateFPUStatus()` ni `updateVPUStatus()` (`arch/riscv/isa.cc:1367-1415`). RVV exige instrucción ilegal para una operación FP vectorial con `FS=Off`, y para cualquier vectorial con `VS=Off`. Debe comprobarse antes de despachar. La parte `VS` afecta también a las rutas de 2.1--2.5. |
| 4. Estado `Dirty` al retirar | Ausente | Al acumular `fflags` (`execute.cc`, fase `Completed`) no se marca `FS=Dirty`; tampoco `VS=Dirty` al escribir el VRF. |
| 5. Captura del redondeo | Presente, sin evidencia | `buildVectorCommand()` captura `frm` para toda ficha con `requiresFpContext` y conserva `Invalid` si `frm` es 5--7; admisión lo rechaza y Minor lo convierte en instrucción ilegal. No consta ejecución. |
| 6. Flags al retirar | Presente para el despacho serie | Se acumulan una vez al retirar la cabeza. Con la ventana de 2.5 deberán retenerse en la entrada joven hasta su retirada. |
| 7. CSRs FP como límite de ventana | Pendiente de 2.5 | Con despacho serie es implícito. La ventana de 2.5 debe cerrarse ante accesos a `frm`, `fflags` y `fcsr`. |

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
| 1. Decode | Ausente | `decodeVectorOp()` no reconoce `vslide1up_vx` ni `vslide1down_vx` (`decode.cc:76-84`), que reciben `IllegalInstFault`. En gem5 son macro-operaciones (`VectorSlide1UpFormat`/`VectorSlide1DownFormat`, `decoder.isa:5741` y `5791`); Minor debe clasificar la macro-op y no despachar sus micro-ops. |
| 2. Construcción del comando | Ausente | `DecodedVectorOp::payload` sólo admite `DecodedArithmeticOp` o `DecodedMemoryOp` (`vector_types.hh:102`). Falta una variante de slide y que `buildVectorCommand()` (`execute.cc`) genere `SlideCommand{operation, destination, vectorSource = vs2, scalar}`. Conviene leer `vd` y `vs2` de los campos de `machInst`, como en el punto 2 de 2.6. |
| 3. Normalización del escalar | Reutilizable | La lectura de `rs1` y `ElementCodec::normalizeScalar(value, xlen, sew)` ya existen para `.vx` (`execute.cc:194-199`). Deben aplicarse una sola vez también al escalar de la slide: extender el signo si XLEN < SEW y conservar los bits bajos si XLEN > SEW. |
| 4. Solapamiento en up | Rechazo en VPU, sin traducción | Admisión rechaza `vslide1up` con `vd == vs2` (`InvalidRegisterGroup`, codificación reservada por RVV). Minor debe convertir ese rechazo en instrucción ilegal antes de efectos, como el resto de rechazos. Down con `vd == vs2` es legal y lo resuelve la VPU. |
| 5. Formas con máscara | Rechazo en VPU | `vm=0` se rechaza en admisión (`UnsupportedConfiguration`). Minor ya captura `masked` del bit 25. |
| 6. Protocolo y retirada | Sin cambios | Se usa el mismo protocolo de reserva, aceptación, finalización y retirada. Minor no crea paquetes de red ni espera entregas individuales; espera la finalización del comando. Una slide no tiene `fpContext` ni produce flags. |
| 7. `mstatus.VS` | Ausente | Igual que el punto 3 de 2.6: la ruta de offload no comprueba `VS=Off` antes de despachar. |

Sin los puntos 1--3 no puede acreditarse el criterio 6 (slides desde
programa, con la red en los casos que cruzan lanes) de la
[sección 15.3][cierre]. El criterio 14 depende sólo de la VPU, pero su
tráfico no se observa en un programa hasta resolver esos puntos.

## Evidencia pendiente común a las fases 1--5

- **Ejecución de programas.** Las entregas 2--5 registran sólo compilación
  dirigida; la fase 5 además enlazó `gem5.opt` y ejecutó `--help`. No hay
  registro de ejecución de una carga con Minor tras los cambios de esas
  fases, ni siquiera del baseline del hito 1 con los nuevos valores por
  defecto (cuatro entradas de cola, cuatro activos y cuatro contextos por
  lane).
- **Compilación de Minor.** Minor no ha cambiado desde la fase 1, cuya
  entrega compiló `minor/cpu.o` y `minor/execute.o`. No se ha comprobado
  que ese código siga compilando con las cabeceras modificadas en 2--5.
- **Matriz CPU--VPU.** No existe la relación por operación, forma, ancho y
  LMUL entre decode, construcción, admisión y retirada que pide la
  fase 2.8 y que cada fase debería actualizar ([sección 13][fases]).

## Fuera del alcance de este informe

No son pendientes de las fases 1--5, aunque aparezcan como `-TODO` en el
código:

- Destinos escalares (`vector_types.hh`, comentario en
  `DecodedArithmeticOp::destination`): hito 3.
- Operandos inmediatos (`.vi`): no forman parte de la matriz del hito 2.
- Offload FP32, Mul/Div/resto y varias ALUs: fase 2.6, con sus
  pendientes CPU en la sección correspondiente de este informe.
- `vslide1up.vx` y `vslide1down.vx`: fase 2.7, con sus pendientes CPU en
  la sección correspondiente de este informe.
- `vmin.vv` y cierre de Pathfinder: fase 2.8.
- Predicación (`vm=0`), chaining y memoria concurrente: hito 3.

## Orden sugerido

Es una sugerencia para preparar propuestas, no un plan aprobado:

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
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
