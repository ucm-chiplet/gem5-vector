# Hito 2: revisión de extremo a extremo entre Minor y la VPU

Paquete P5 del [plan de cierre de la fase 8][plan-f8], 11 de noviembre de
2026, sobre `709eda530f`. Es una revisión de fuentes: no modifica código
ni ejecuta cargas. Contrasta el despacho concurrente de Minor
([diseño de concurrencia][concurrencia-minor]) con el frontend de la VPU,
siguiendo la sección 7 del [diseño del hito][diseno] y los puntos que los
[pendientes de Minor][pendientes-minor] dejaban abiertos.

## Resultado

No se encuentran defectos de corrección. El recorrido conserva el orden,
la identidad, las dependencias y el aplazamiento de interrupciones con la
ventana de Minor. Quedan cinco matices de semántica que conviene conocer
al interpretar métricas o ampliar la concurrencia.

## Puntos revisados

| Aspecto | Conclusión | Código |
| --- | --- | --- |
| Orden de despacho | Minor recorre la ventana en orden de programa y se detiene en el primer bloqueo (consecutividad, tipo, rechazo, `vstart`, `Stall`). Una instrucción joven nunca se despacha antes que una anterior. | `Execute::advanceVectorDispatch` |
| Dependencias vectoriales | Minor no las comprueba y las delega en la VPU. Es seguro: los comandos llegan en orden a una cola FIFO y el secuenciador inicia en orden con un scoreboard RAW/WAR/WAW conservador. | `AraSequencer::blockersFor`, `VectorScoreboard` |
| Liberación anticipada | La VPU libera el scoreboard al terminar, antes de que Minor retire. Es seguro porque en la ventana sólo entran instrucciones aritméticas, que no pueden fallar después de aceptarse, y tras un rechazo no se despacha ninguna posterior. | `ara_sequencer.cc` (`scoreboard.release`), `prev_state.rejected` |
| Rechazo de una instrucción joven | Pasa a `Completed` con su motivo y se convierte en `IllegalInstFault` al llegar a la cabeza, después de retirar las anteriores. | `commitInst` |
| Captura de operandos | La instrucción joven captura configuración y escalares al despacharse, antes de que la anterior retire. Es correcto: la regla de consecutividad excluye cualquier escritor intermedio de `vl`, `vtype` o de un escalar, y los productores escalares anteriores ya han retirado. | `buildVectorCommand` |
| Orden con memoria escalar | La memoria vectorial sólo se despacha en cabeza y con el store buffer vacío; `hasPendingVpuStoreBefore` retiene los accesos escalares posteriores a un store vectorial. | `advanceVectorDispatch`, `hasPendingVpuStoreBefore` |
| Interrupciones | `isInbetweenInsts` exige que ninguna entrada tenga obligaciones (`mustCompleteVpuOffload`), así que una interrupción espera a que termine todo lo despachado, también lo joven. Sólo se ejercita en FS; en SE no hay controlador. | `Execute::isInbetweenInsts`, `MinorVectorState::mustComplete` |
| Drain | No se despacha durante el drain e `isDrained` exige la ventana vacía. | `advanceVectorDispatch`, `Execute::isDrained` |
| Identidad | `CommandKey` se asigna una vez por instrucción y los callbacks la buscan en toda la ventana. | `accepted`, `completed` |
| Contrapresión | `Stall` en la reserva detiene el despacho; la cola de la VPU acota los comandos aceptados. | `requestGrant` |

## Matices de semántica

1. **Ventana por posiciones.** `maxActiveVectorOffloads` limita las
   primeras entradas de `vpuOffloadStates`, incluidas las ya completadas
   que esperan retirada. Una instrucción joven terminada ocupa sitio hasta
   que la anterior retira. El nombre sugiere "comandos activos".
2. **Las slides no entran en la ventana.** Sólo se solapan instrucciones
   aritméticas consecutivas; una slide espera a ser cabeza aunque la VPU
   pudiera ejecutarla junto a una aritmética independiente. Es una
   decisión conservadora coherente con el diseño de 2.5.
3. **`vectorBlockedCapacity` cuenta evaluaciones con la ventana llena**,
   aunque no haya ninguna instrucción que despachar fuera de ella.
4. **Intervalos abiertos en los volcados.** El secuenciador cierra sus
   intervalos de espera y de ocupación en el siguiente cambio; un
   intervalo abierto al volcar estadísticas se contabiliza en el periodo
   siguiente. Un volcado de región de medida puede perder la cola final.
5. **Dos ventanas.** Con la ventana de Minor en 2 por defecto, los 4
   comandos activos que admite la VPU no se alcanzan desde un programa;
   hay que subir `maxActiveVectorOffloads` para estudiarlos.

Los matices 3 y 4 afectan a la observabilidad y se tratan junto con las
correcciones de métricas del [análisis de observabilidad][observabilidad].
Los demás no requieren cambios en el hito 2.

[plan-f8]: <Plan de cierre de la fase 8.md>
[concurrencia-minor]: <Diseno de concurrencia MinorCPU - Fase 2.5.md>
[diseno]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md>
[pendientes-minor]: <Pendientes de Minor - Fases 1 a 5.md>
[observabilidad]: <Observabilidad - análisis con Pathfinder.md>
