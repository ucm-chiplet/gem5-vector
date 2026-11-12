# Entrega al hito 3: contratos y campaña de micro-kernels

Paquete P8 del [plan de cierre de la fase 8][plan-f8] (puntos 3 y 7 de la
sección 13.8 del diseño), 11 de noviembre de 2026, sobre `c2f5e9b557`.
Sirve de punto de partida para las propuestas del
[hito 3 del plan de evolución][hito3]. No implementa nada: describe qué
existe hoy, qué está sólo preparado y qué tendrá que ampliar cada fase.

Distingue tres niveles:

- **Activo:** el código lo usa y hay un camino desde un programa.
- **Preparado:** existe un tipo, campo o interfaz, pero no hay productor
  ni consumidor, o la admisión lo rechaza. Un contrato preparado no
  acredita soporte.
- **Ausente:** no hay representación.

Las rutas son relativas a `src/cpu/vector_engine/` salvo las de Minor.

## 1. Mapa de contratos

| Capacidad (fase del hito 3) | Estado actual | Dónde | Qué hay que ampliar | Responsable |
| --- | --- | --- | --- | --- |
| Disponibilidad por rangos y chaining (3.3) | Activo sólo a nivel de registro: el scoreboard reserva el grupo completo (el contenedor en LMUL fraccional) y lo libera al terminar el comando; un consumidor espera a todo su productor | `frontend/vector_scoreboard.hh` (`VectorScoreboard`), `frontend/ara_sequencer.cc` (`blockersFor`, `scoreboard.release`) | Publicar rangos de elementos o bytes válidos ligados a la identidad del productor (`TaskKey`, `VrfAccessKey`) y emitir tareas consumidoras por rango. WAR y WAW pueden seguir siendo conservadores | Frontend y VRF |
| Predicación (3.2) | Preparado: `VectorConfig::masked`, `OperandRole::Predicate`, `ElementFormat::Mask`. La admisión rechaza `masked` | `common/vector_types.hh`, `common/operand_types.hh`, `common/vector_support.cc` (`!config.masked`) | Leer `v0` como operando con su propia dependencia, escrituras parciales por elemento, `mu/ma`, y transporte de la máscara a lanes y red | Minor (decode `vm`), soporte, lanes, VRF |
| Tercer operando y acumulador (3.1; AXPY) | Ausente en los comandos: `ArithmeticCommand` tiene `vectorSource` y `secondOperand`. Preparado en descriptores: `OperandRole::Accumulator`, `OperandAccess::ReadWrite` | `interface/vector_command.hh`, `common/operand_types.hh` | Leer el destino como fuente (`vfmacc`), escalar FP capturado en el comando, FP64 en la FPU, y reservar su lectura en el scoreboard | Minor, contratos, FPU |
| Resultados escalares (3.1, 3.6) | Preparado: `ScalarResult` en `VectorCompletion` y `ScalarDestination`. `CpuVectorInterface::completed` rechaza (`panic`) una finalización con resultado escalar | `interface/vector_completion.hh`, `common/operand_types.hh`, `interface/cpu_vector_interface.cc` | Productor en la VPU, escritura del registro en Minor al retirar y dependencia escalar para las instrucciones posteriores | Minor, interfaz, unidad productora |
| Colectivos (3.6) | Ausente: `TransferKind` sólo tiene `RegisterMove`; no hay estado colectivo | `common/interconnect_types.hh` | Contribuciones por lane, estado parcial, destino final, terminación y liberación; orden de combinación separado del transporte | Nueva unidad colectiva, red |
| Memoria concurrente (3.4) | Activo con límites: la memoria es barrera en el secuenciador; la VLSU tiene un bloque activo y el backend un paquete en vuelo; Minor despacha memoria sólo en cabeza con el store buffer vacío | `frontend/ara_sequencer.cc` (`memoryBarrier`), `vpu/vlsu/ara_vlsu.hh`, `vpu/vlsu/vector_memory_backend.hh`, `src/cpu/minor/execute.cc` (`advanceVectorDispatch`, `hasPendingVpuStoreBefore`) | Tres capacidades separadas (comandos, bloques, peticiones), orden load/store y con memoria escalar, alias, traducciones y reintentos | Minor, secuenciador, VLSU, backend |
| Direccionamiento strided e indexado (preparación 3.1) | Preparado: `StridedAddress` e `IndexedAddress` en `MemoryCommand`; la admisión exige unit-stride | `interface/vector_command.hh`, `common/vector_support.cc` (`supportsMemory`) | Descriptores de stride, índices y anchos de índice sin activarlos (strided en el 4, indexado en el 5) | Contratos |
| Faults precisos con resultados jóvenes (3.5) | Activo para un comando: `FaultInfo` con índice y `finalVstart`; un comando con `vstart` ≠ 0 se ejecuta aislado | `interface/vector_completion.hh`, `frontend/ara_sequencer.cc` (`isolation`) | Impedir que un resultado joven se publique antes de saber que una memoria anterior no falla; revisar el aislamiento con varias instrucciones de memoria | Minor, secuenciador |
| Progreso escalar en Minor (3.5) | Activo con límites: ventana por posiciones (`maxActiveVectorOffloads=2`), sólo aritmética consecutiva; slides y memoria en cabeza | `src/cpu/minor/execute.cc` (`advanceVectorDispatch`) | Despachar comandos que usan escalares producidos mientras otro comando vectorial sigue activo, con publicación arquitectónica segura | Minor |
| Clústeres (3.7) | Preparado en parte: `VectorInterconnect` es independiente de topología y `AddressMapper` decide la ubicación; la identidad de lane es plana (`LaneId`) | `vpu/interconnect/vector_interconnect.hh`, `vpu/register_file/address_mapper.hh`, `common/vrf_types.hh` | Separar identidad de ubicación (clúster y lane local), transporte local frente a remoto y finalización global | Mapper, red, distribuidor |

## 2. Garantías del hito 2 que dependen de estos límites

Al ampliar una fila hay que revisar las garantías que se apoyan en ella
([revisión de extremo a extremo][revision]):

- **Liberación anticipada del scoreboard.** La VPU libera las reservas al
  terminar, antes de que Minor retire. Es seguro porque en la ventana sólo
  entran instrucciones aritméticas, que no fallan tras aceptarse. Deja de
  valer en cuanto la memoria entre en la ventana (3.4, 3.5).
- **Captura de operandos.** Una instrucción joven captura `vl`, `vtype` y
  escalares al despacharse; la regla de consecutividad excluye escritores
  intermedios. El progreso escalar (3.5) necesita otra garantía.
- **Orden de memoria.** La barrera del secuenciador y el despacho en cabeza
  sustituyen hoy a cualquier análisis de alias.
- **Interrupciones y drain.** Esperan a que termine todo lo despachado;
  con memoria concurrente habrá más estado en vuelo que drenar.
- **Observabilidad.** Las métricas de solapamiento (`sequencer.*`) miden
  comandos completos. Chaining necesitará métricas por rango (tiempo hasta
  el primer resultado, solapamiento productor--consumidor). Un intervalo
  abierto al volcar se cuenta en el periodo siguiente (matiz 4 de la
  revisión, pendiente).

## 3. Datos de partida medidos en el hito 2

Del [análisis de observabilidad][observabilidad], con la configuración por
defecto y SEW 32, m1:

- La VLSU ocupa el 54 % de Pathfinder `tiny` y el 83 % del micro-kernel de
  concurrencia, con un bloque y un paquete en vuelo. Es el recurso que más
  limita: apoya empezar el hito 3 por la memoria concurrente (3.4).
- Pathfinder es una cadena de dependencias: no hay solapamiento entre
  comandos (`sequencer.overlappedStarts=0`). Es candidato natural para
  medir chaining (3.3).
- En el micro-kernel, la segunda ALU sólo se usa cuando los buffers de la
  clase ALU tienen 2 entradas; aumentar unidades sin buffers no tiene
  efecto.
- Con la ventana de Minor en 2, los 4 comandos activos de la VPU no se
  alcanzan.

Son ticks del modelo, no una calibración frente a Ara.

## 4. Ficha de campaña para el hito 3

Propuesta para el equipo; ninguno está implementado ni autorizado. Cada
micro-kernel seguiría el patrón de [`benchmarks/concurrency/`][concurrency]:

- **ROI:** la función `runKernel()` (`noinline`) entre `vpuRoiBegin()` y
  `vpuRoiEnd()` de `benchmarks/common/vpu_runtime.h`; el primer volcado de
  `stats.txt` es la región medida.
- **Ejecución:** runner común `benchmarks/common/vpu_bench.py`, modos
  offload y native con huella idéntica, verificación escalar completa.
- **Perfil:** `check_profile.py` exige los tipos RVV previstos en
  `runKernel`; Clang con `-fno-vectorize -fno-slp-vectorize`, sin libc.
- **Comparación:** cada mecanismo se mide activado y desactivado con la
  misma entrada y configuración, para atribuirle la diferencia.

| Micro-kernel | Pregunta | Instrucciones | Métricas | Comparación | Requiere |
| --- | --- | --- | --- | --- | --- |
| Cadena productor--consumidor | ¿Empieza el consumidor antes de terminar el productor? | `vle32`, `vmul.vv` → `vadd.vv` → `vse32`, con `vl` grande y LMUL m4/m8 | Tiempo hasta el primer resultado, solapamiento productor--consumidor, espera por rango frente a `hazardWaitTicks` | Chaining activado/desactivado | 3.3 |
| Loads independientes | ¿Hay varias instrucciones de memoria en vuelo? | Varias `vle32` a registros y direcciones distintos | Comandos, bloques y peticiones en vuelo; `vlsu.memoryTicks` | Límites de comandos, bloques y peticiones | 3.4 |
| Load--cálculo--store | ¿Se solapan memoria y cálculo? | `vle32`, `vadd.vv`, `vse32` en flujo | Ocupación VLSU frente a ALU, ticks con ambos activos | Barrera de memoria activada/desactivada | 3.4, 3.5 |
| Alias load/store | ¿Se conserva el orden cuando no se prueba independencia? | Store y load a direcciones solapadas y disjuntas | Resultado; esperas por orden | Direcciones solapadas frente a disjuntas | 3.4 |
| Predicación | ¿Respetan las escrituras parciales la máscara? | `vadd.vv` y slides con `vm=0`, densidades 0 %, 50 %, 100 % y patrones | Elementos activos, accesos al VRF, `mu` frente a `ma` | Con y sin máscara | 3.2 |
| Progreso escalar | ¿Avanza Minor con comandos vectoriales pendientes? | Bucle con aritmética escalar independiente entre comandos largos | Ciclos de Minor no bloqueados, capacidad vectorial sin usar | Ventana de Minor 2 y 4 | 3.5 |
| AXPY (RiVEC) | Cierre del hito 3 | `vsetvli`, `vle64`, `vse64`, `vfmacc.vf` | Reparto como en Pathfinder, flags FP al retirar | Chaining y memoria concurrente activados/desactivados | 3.1, 3.3--3.5, FP64 |

Los pendientes de evidencia del hito 2 (anchos, LMUL, Mul/Div/resto, FP32
completo, slides, rechazos, retry y faults) tienen su propia
[campaña sugerida en el cierre][cierre-campana] y no se repiten aquí.
Conviene cubrir los de anchos y LMUL antes que la cadena productor--consumidor,
que los usa.

## 5. Decisiones abiertas para las propuestas del hito 3

- Granularidad de la disponibilidad: elemento, palabra por lane o
  fragmento de tarea.
- Si WAR y WAW siguen siendo conservadores o se introduce renombramiento.
- Política load/store inicial: orden total, por direcciones o por bloque.
- Dónde vive el estado colectivo y quién lo libera.
- Si la máscara viaja como operando del VRF o como dato de la red.
- Cómo se publica un resultado joven cuando una memoria anterior aún puede
  fallar.
- Si la ventana de Minor pasa a contar comandos activos en lugar de
  posiciones.

[plan-f8]: <Plan de cierre de la fase 8.md>
[hito3]: <../Plan de evolución - Hitos 2 a 5.md#hito-3-solapamiento-escalar-vectorial-chaining-y-memoria-concurrente>
[revision]: <Revisión de extremo a extremo - Minor y VPU.md>
[observabilidad]: <Observabilidad - análisis con Pathfinder.md>
[concurrency]: <../../benchmarks/concurrency/README.md>
[cierre-campana]: <Cierre del hito 2 - Estado de los criterios.md#campaña-de-validación-sugerida>
