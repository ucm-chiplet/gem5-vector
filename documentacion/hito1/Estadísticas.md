# Estadísticas de la VPU: hito 1

Las estadísticas observan el trabajo de los módulos actuales. No cambian
el soporte de instrucciones, la admisión, los mensajes ni el calendario de
eventos. Se registran como grupos nativos de gem5 y aparecen en `stats.txt`
con su descripción y unidad. El benchmark también las exporta a CSV.

Esta referencia incluye los contadores añadidos por la [entrega VPU de
fase 2 del hito 2](<../hito2/Fase 2 - Anchos y grupos.md>). Esa ampliación
no añade evidencia funcional al baseline ni a las nuevas combinaciones.

## Dónde encontrarlas

```bash
python3 benchmarks/baseline/run.py
```

El runner indica la carpeta local de resultados. Lee primero `summary.md`;
consulta después `offload/vpu-statistics.csv` para la VPU o `stats.txt` para
el sistema completo. El modo `native` no instancia la VPU y su CSV sólo
contiene cabecera. Los resultados se guardan bajo `benchmarks/baseline/results/`
y no se suben a GitHub. Véase la [guía del benchmark](../../benchmarks/baseline/README.md).

La raíz de la VPU es `system.cpu.vectorEngine`. La interfaz de Minor está
bajo `system.cpu.vectorInterface`. Los índices de lanes y bancos empiezan
por cero y corresponden a la configuración realmente instanciada.

| Grupo relativo a `vectorEngine` | Módulo | Una unidad de trabajo significa |
| --- | --- | --- |
| `commands` | VectorEngine | Comando despachado; termina al devolver la finalización. |
| `queue` | CommandQueue | Reserva concedida; conserva crédito durante FIFO y ejecución, hasta `releaseHead`. |
| `sequencer` | AraSequencer | Comando que comienza a ejecutarse; incluye rangos vacíos. |
| `distributor` | TaskDistributor | Tarea aritmética; termina tras todos sus fragmentos. |
| `lane<N>` | AraLane | Fragmento local; termina después del writeback confirmado. |
| `lane<N>.alu` | Alu | Bundle entero; termina al producir el resultado. |
| `lane<N>.fpu` | Fpu | Bundle binary16 interno; termina al producir resultado y flags. |
| `lrf<N>` | LaneRegisterFile | Lectura o escritura aceptada en la slice local. |
| `lrf<N>.bank<M>` | VectorRegBank | Acceso aceptado a una palabra del banco. |
| `vlsu` | AraVLSU | Tarea de carga/store; termina al cerrar todos sus elementos o un fault. |
| `memory` | VectorMemoryBackend | Petición lógica de un elemento; termina tras memoria o fault. |
| `mapper` | AddressMapper | Invocación software del mapeo; no es un acceso hardware. |

`vectorInterface` cuenta comandos despachados desde Minor, y termina su
intervalo al recibir `completed`. Los tipos de `common/` y `VectorSupport`
son contratos y helpers, no recursos temporizados independientes.
`interconnect.enabled = 0` indica que el transporte entre lanes sigue sin
implementar; no se atribuyen paquetes ni ocupación a un recurso inexistente.

## Contadores comunes

`common/module_stats.hh/.cc` conserva únicamente contabilidad. No programa
eventos ni participa en decisiones funcionales.

| Sufijo | Unidad | Condición y significado |
| --- | --- | --- |
| `accepted` | Count | Incrementa una vez al admitir la unidad de trabajo de la tabla. No cuenta reintentos. |
| `completed` | Count | Incrementa al cerrar esa unidad, antes del callback externo; incluye faults terminales. |
| `retries` | Count | Número de intentos fallidos de admisión. En engine/interfaz/cola representa Stall; en sequencer, Retry del backend. |
| `rejected` | Count | Rechazos permanentes del descriptor. Se usa en frontera y cola; los invariantes internos siguen sus panics habituales. |
| `faults` | Count | Unidades cerradas con fault. La cola libera crédito sin procesar el status, por lo que su `faults` permanece a cero. |
| `elements` | Count | Elementos del rango admitido, incluidos los de un trabajo que pueda fallar después. |
| `bytes` | Byte | Bytes del payload admitido; se cuentan una vez por unidad y no por retry. |
| `outstanding` | Count | Unidades actualmente admitidas y todavía no cerradas. Es un valor actual, no un acumulado. |
| `peakOutstanding` | Count | Máximo de trabajo pendiente desde el último reset de estadísticas. |
| `busyTicks` | Tick | Tiempo simulado con al menos una unidad pendiente. |
| `workTicks` | Tick | Integral del número de unidades pendientes; incluye intervalos aún abiertos al volcar. |

En comandos/tareas/bundles, `elements` cubre `[vstart, vl)` y `bytes` su
rango de datos. No se multiplican los bytes por el número de operandos.
En el VRF, `elements` es cero porque la interfaz describe bytes; `bytes`
cuenta sólo los bytes habilitados de la palabra. En memoria, cada petición
lógica admitida cuenta un elemento y su tamaño. El mapper cuenta el tamaño
del rango examinado y deja `elements` a cero; incluye llamadas realizadas
para validar y repartir trabajo, por lo que no mide tráfico de VRF.

`busyTicks` se mide entre admisión y cierre y contiene todas las esperas del
módulo. `workTicks` puede superar `busyTicks` cuando hay varios accesos
pendientes, por ejemplo en bancos distintos del LRF. El mapper realiza su
trabajo en el mismo tick: ambos tiempos son cero. Los tiempos de ALU/FPU
son las demoras funcionales del modelo, no latencias calibradas de Ara.

## Métricas específicas

| Módulo | Métricas | Interpretación |
| --- | --- | --- |
| Interfaz CPU | `keysAllocated`, `grants`, `acceptedNotifications` | Identidades asignadas, reservas concedidas e inserciones FIFO confirmadas. Una identidad sin grant puede descartarse. |
| Cola | `dispatched`, `capacityStalls`, `drainStalls` | Consumo de reservas e intentos bloqueados. Si drain y capacidad coinciden, se atribuye el intento a drain. |
| Sequencer | `arithmeticCommands`, `loadCommands`, `storeCommands`, `emptyCommands` | Familias de comandos iniciados; un rango vacío se clasifica por familia y además no genera tarea. |
| Distribuidor | `fragmentsCreated`, `fragmentsAccepted`, `fragmentsCompleted`, `laneRetries` | Fragmentación y entregas por lane. Un retry no crea fragmento nuevo. |
| Lane | `vrfReads`, `vrfWrites`, `vrfReadRetries`, `vrfWriteRetries`, `executionRetries` | Accesos concedidos y reintentos en cada frontera; un escalar de `.vx` no provoca lectura de segunda fuente vectorial. |
| ALU | `elements8`, `elements16`, `elements32`, `elements64` | Elementos admitidos por ancho; 8/64 se añaden en la entrega VPU de fase 2 del hito 2. El baseline comprobado utiliza 32 bits. |
| FPU | `inexactBundles`, `underflowBundles`, `overflowBundles`, `divideByZeroBundles`, `invalidBundles` | Bundles completados con cada flag, como máximo una vez por flag y bundle. Son causas no excluyentes. |
| LRF/banco | `reads`, `writes`, `readBytes`, `writeBytes` | Accesos aceptados y bytes habilitados por dirección; excluyen intentos rechazados. |
| VLSU | `loadTasks`, `storeTasks` | Tareas admitidas por dirección. |
| VLSU | `loadElements`, `storeElements`, `loadBytes`, `storeBytes` | Elementos y bytes terminados con éxito. Una carga requiere WriteAck del VRF y un store StoreAck de memoria. |
| VLSU | `memoryRequests`, `memoryRetries`, `vrfReadRetries`, `vrfWriteRetries` | Entregas lógicas y reintentos en backend/VRF. |
| Mapper | `fragments` | Fragmentos de palabra devueltos por las llamadas software. |
| Memoria | `loads`, `stores`, `fragmentsCreated`, `translations` | Peticiones lógicas admitidas, fragmentos físicos preparados y traducciones iniciadas. |
| Memoria | `translationFaults`, `accessFaults` | Faults de MMU y errores del puerto, separados. |
| Memoria | `sendAttempts`, `packetsSent`, `packetBytes`, `packetResponses` | Intentos timing, paquetes aceptados, sus bytes y respuestas físicas. |
| Memoria | `portRetries`, `retryNotifications` | Envíos rechazados y avisos `recvReqRetry`; no son peticiones lógicas nuevas. |

La FPU interna está instrumentada, pero el benchmark entero no la utiliza.
Sus valores a cero son esperables: no indican que Minor tenga soporte de
instrucciones FP vectoriales.

## Cómo comprobar coherencia

En una ejecución completa sin reset ni faults:

- `accepted == completed` y `outstanding == 0` en cada grupo de trabajo.
- Los comandos completados concuerdan en interfaz, engine, cola y sequencer.
- Las tareas aritméticas del distribuidor concuerdan con esa familia del
  sequencer; las tareas de memoria concuerdan con cargas más stores.
- Los fragmentos creados, aceptados y terminados del distribuidor concuerdan
  con la suma de fragmentos completados de todas las lanes.
- En cada ALU, `elements8 + elements16 + elements32 + elements64` coincide
  con `elements`: los cuatro contadores registran admisiones, sin retries.
- `lrf<N>.accepted` concuerda con sus lecturas más escrituras y con la suma
  de accesos aceptados en sus bancos.
- `memory.accepted == vlsu.memoryRequests`; los elementos de carga/store
  terminados concuerdan con las peticiones lógicas cuando no hay faults.
- `memory.sendAttempts == memory.packetsSent + memory.portRetries`;
  `packetResponses == packetsSent` al vaciar el backend.
- Los bytes de paquetes coinciden con los bytes lógicos terminados si no
  hay faults. La fragmentación puede aumentar el número de paquetes.

Estas son relaciones entre fronteras, no totales para sumar. Un elemento
puede contarse en comando, tarea, lane y ALU. Los bytes del VRF cuentan
operandos y writebacks; los bytes de paquetes cuentan el puerto del backend.
Ninguno de esos contadores mide por sí solo tráfico DRAM.

El tiempo de cada nivel contiene tiempo de sus submódulos. No se deben sumar
`busyTicks` entre niveles para obtener la duración global, ni sumar flags o
causas de retry como si fueran ciclos de bloqueo.

## Dumps, resets y límites de validación

El tiempo se actualiza al cambiar la ocupación y en `preDumpStats`, incluidos
los intervalos abiertos. No se necesita sondear los módulos cada ciclo.
`resetStats` reinicia acumulados y el origen temporal, conservando el trabajo
real en curso en `outstanding` y en el máximo inicial. Por ello, después de
un reset con trabajo activo, `completed` puede superar `accepted`: las
admisiones anteriores pertenecen al intervalo previo. El balance correcto
incluye la ocupación inicial. Los dumps repetidos no duplican tiempo.

El benchmark compara los resultados con una referencia escalar independiente
para ambos modos. La comprobación de esta instrumentación también revisa
balances, elementos, bytes y paquetes de su ejecución. Los ticks, instrucciones
y operaciones simuladas se comparan con la ejecución anterior a instrumentar.
Esta validación no ejercita la FPU interna, faults, resets en mitad de trabajo
ni todos los escenarios de drain; no se incorporan nuevas suites al repositorio.
Los experimentos del [hito 2](../hito2/README.md) exigirán métricas adicionales
cuando existan sus recursos temporizados.
