# Diseño de Concurrencia MinorCPU (Fase 2.5)

Este documento formaliza la propuesta y diseño de la concurrencia CPU
requerida en la Fase 2.3 (Punto 1 / Acción 7) y en la Fase 2.5 del Hito 2:
desacoplo de despacho y retirada en `MinorCPU` para permitir la emisión
superpuesta de instrucciones vectoriales independientes hacia la VPU
manteniendo la semántica secuencial y la precisión arquitectónica.

---

## 1. Motivación y Objetivos

En el diseño original del Hito 1, `MinorCPU` gestionaba las instrucciones
de offload vectorial de forma estrictamente serie:
1. Una instrucción vectorial sólo iniciaba su protocolo de admisión
   (`requestGrant`) cuando alcanzaba la cabeza de la cola de retirada
   (`inFlightInsts.front()`).
2. La CPU retenía la etapa de Execute y no despachaba ninguna otra
   instrucción vectorial hasta que la que estaba en vuelo finalizaba
   completamente (`Completed`) y se retiraba de esta cola.

Dado que la VPU Ara-like cuenta con recursos internos para procesar múltiples
comandos concurrentes (por defecto hasta `max_active_commands = 4`), el
despacho serie en la CPU impedía que cualquier programa real alimentara dicha
concurrencia.

El objetivo de este diseño es **desacoplar el avance del despacho del punto
de retirada**, permitiendo mantener hasta $N$ comandos vectoriales
concurrentes en vuelo hacia la VPU, garantizando:
* **Preservación del orden de programa** en efectos arquitectónicos y traps.
* **Aislamiento ante excepciones o rechazos** de instrucciones jóvenes.
* **Resolución asíncrona de callbacks** mediante identidades de comando.
* **Seguridad ante contrapresión, descartes y drain** del procesador.

---

## 2. Dimensionamiento de la Ventana y Configuración

### Capacidad finita de seguimiento
La concurrencia en la CPU no es ilimitada:
* Está acotada por un parámetro configurable de la CPU:
  `maxActiveVectorOffloads` (declarado en `BaseMinorCPU.py` con valor
  predeterminado 2 para la configuración de Fase 2.5).
* Este parámetro define el número máximo de comandos vectoriales que pueden
  encontrarse simultáneamente en estados activos de VPU (`Dispatched` o
  `Accepted`) dentro de la estructura de seguimiento `vpuOffloadStates`.

### Tasa de despacho configurable
Para controlar las ráfagas sobre el puerto de admisión y modelar anchos de
interfaz configurables:
* La tasa está acotada por el parámetro `vectorDispatchWidth` de `MinorCPU`
  (con valor predeterminado 1 en `BaseMinorCPU.py`), limitando cuántos
  comandos vectoriales pueden transicionar a `Dispatched` en un mismo ciclo
  de reloj (`num_dispatched_this_cycle < vectorDispatchWidth`). -> Se puede parametrizar y que el desarollador elija el ancho de dispatch a la VPU.

---

## 3. Protocolo de Despacho Desacoplado (`advanceVectorDispatch`)

El avance de despacho se ejecuta de manera independiente al ciclo de retiro:

```
+-------------------------------------------------------------------------+
| commitInst()                                                            |
|                                                                         |
| 1. Evalúa si la cabeza de inFlightInsts es vectorial.                  |
| 2. Invoca advanceVectorDispatch(thread_id).                             |
|    - Itera sobre vpuOffloadStates mientras active_in_vpu < MaxOffloads. |
|    - Evalúa condiciones de apertura sobre instrucciones sucesoras.      |
|    - Transiciona: ReadyToGrant -> WaitGrant -> Dispatched.              |
| 3. Procesa el retiro exclusivo de la cabeza en inFlightInsts.          |
+-------------------------------------------------------------------------+
```

### Transición inmediata sin burbujas artificiales
Dentro de un mismo ciclo, si una instrucción en `ReadyToGrant` obtiene
concesión inmediata (`GrantResult::Granted`), pasa a `WaitGrant` e
inmediatamente después a `Dispatched` invocando `dispatch()`, evitando
añadir latencias estructurales artificiales de ciclo entre la concesión
y el despacho.

---

## 4. Barreras y Condiciones de Apertura Concurrente

Para que una segunda instrucción vectorial joven pueda despacharse mientras
la primera sigue en vuelo en la VPU, deben cumplirse rigurosamente las
siguientes condiciones en MinorCPU:

1. **Orden estricto de programa consecutivo:**
   La instrucción joven $i$ debe ser inmediatamente contigua en la secuencia
   de ejecución a la instrucción precedente $i-1$:
   ```cpp
   states[i].inst->id.execSeqNum == states[i-1].inst->id.execSeqNum + 1
   ```
   Cualquier instrucción escalar, salto, cambio de flujo o excepción
   intermedia cierra la ventana.
2. **Naturaleza de la operación (exclusivamente aritmética):**
   Todas las instrucciones vectoriales involucradas deben ser de clase aritmética (`DecodedArithmeticOp`).
   Las instrucciones de memoria (`vle*.v`, `vse*.v`) actúan como barrera
   absoluta en Minor y no se despachan concurrentemente.
3. **Estado de error o rechazo limpio:**
   La instrucción precedente no debe haber sido rechazada por la VPU
   (`!states[i-1].rejected`) ni haber experimentado fallos.
4. **Vector Start nulo (`vstart == 0`):**
   Sólo se permite concurrencia con operaciones completas desde el elemento 0.
   Un `vstart != 0` aísla la ejecución.
5. **Configuración CSR inmutable:**
   No se admiten instrucciones de configuración vectorial (`vsetvl*`) entre
   comandos concurrentes.

---

## 5. Captura Única e Inmutabilidad ante Contrapresión

* El descriptor `MinorVectorState` se crea tempranamente en la etapa de Issue.
* Al despachar, el comando `VectorCommand` captura sus operandos escalares
  (como el valor `rs1` para formas `.vx`) y su `CommandKey` única.
* **Invariante de contrapresión:** Si admisión responde con contrapresión
  (`GrantResult::Stall`), el estado se mantiene en `WaitGrant` y no permite requestGrant de instrucciones posteriores (sale del búcle de dispatch), conservando el
  comando construido de forma inmutable. En los siguientes ciclos, el
  reintento reutiliza dicho comando sin recapturar registros ni regenerar
  la clave.

---

## 6. Correlación Asíncrona por Clave (`CommandKey`)

Cuando la VPU emite respuestas a través de los callbacks de interfaz,
éstas no asumen correspondencia con la cabeza de la cola:
* `accepted(CommandKey, ...)`: Localiza la instrucción correspondiente
  recorriendo `vpuOffloadStates` por coincidencia de `commandKey`, pasando
  su fase a `Accepted` y registrando el identificador de tarea asignado.
* `completed(CommandKey, CompletionResult)`: Localiza la instrucción por
  `commandKey`, almacena el resultado en su campo `completion` y transiciona
  la fase a `Completed`.

---

## 7. Retiro en Orden y Tratamiento Terminal de Errores

Aunque las instrucciones pueden completar asíncronamente en la VPU:
* **El retiro (`commitInst`) se mantiene estrictamente en orden de programa:**
  Sólo la instrucción a la cabeza de `inFlightInsts` puede retirar.
* Si una instrucción joven completa antes que su predecesora, su resultado
  queda retenido en su `MinorVectorState` hasta que le llegue el turno de
  retirar.
* Al retirar, se evalúa exhaustivamente el `CompletionStatus`:
  - `Success`: commit ordinario e incremento de estadísticas.
  - `MemoryFault`: invocación de la falta en el ThreadContext.
  - `IllegalInstruction`: generación e invocación de `IllegalInstFault`.
  - `Cancelled`: descarte limpio sin efectos arquitectónicos ni commit.
  - `InternalError`: emisión de pánico descriptivo con contexto del comando.
* Si una instrucción es rechazada (`Rejected`), la CPU retiene el motivo
  (`rejectionReason`) y genera un `IllegalInstFault` al alcanzar el retiro,
  asegurando que las instrucciones previas hayan completado con éxito.

---

## 8. Seguridad de Vaciado (Drain) y Descartes

* **Protección de descarte:** La función `mustCompleteVpuOffload()`
  inspecciona todas las entradas activas de `vpuOffloadStates`. Si alguna
  instrucción ha realizado reserva o ha sido despachada, no puede ser
  descartada en un squash sin esperar su resolución.
* **Drain seguro:** El método `isDrained()` de la etapa de Execute verifica
  que `vpuOffloadStates` se encuentre completamente vacío antes de confirmar
  el vaciado del procesador.

---

## 9. Observabilidad y Estadísticas de Concurrencia

En `MinorStats` se añaden métricas para auditar la ventana concurrente:
* `vectorMultiActiveCycles`: Ciclos con $\ge 2$ comandos simultáneamente
  activos en la VPU (no cuentan completed).
* `vectorActiveCommandsDistribution`: Vector con la distribución de ciclos
  según el número de comandos activos en vuelo ($0, 1, 2\dots$).
* Contadores de causa de bloqueo que cerraron la ventana concurrente:
  - `vectorBlockedCapacity`: Superado el límite de ventana
    (`maxActiveVectorOffloads`).
  - `vectorBlockedOrder`: Secuencia no consecutiva (`execSeqNum`).
  - `vectorBlockedStructural`: Operación no puramente aritmética.
  - `vectorBlockedVstart`: Instrucción con `vstart != 0`.
