# Hito 2: responsabilidades de CPU y VPU

Guía de trabajo por fase, complementaria al [diseño del hito][diseno] y a
su [matriz de instrucciones y anchos][hito2]. La [entrega de fase 1][fase1]
distingue contratos implementados y capacidades pendientes. Las acciones
siguientes describen responsabilidades; no certifican soporte comprobado.
La [entrega VPU de fase 2](<Fase 2 - Anchos y grupos.md>) registra la
ampliación de anchos y grupos del backend, sus comprobaciones y los
pendientes de integración CPU; no cierra la fase conjunta por sí sola.

El contexto y los límites se mantienen en los [objetivos globales][objetivos]
(O2 y O6), el [resumen de hitos][resumen], el [plan de evolución][plan] y el
[plan de benchmarks][benchmarks]. Esta guía no amplía su alcance. Las
comprobaciones indicadas son propuestas para acordar con el equipo.

## Tabla de responsabilidades

CPU incluye la integración con Minor y las rutas ISA necesarias para
traducir instrucciones a comandos. VPU incluye frontend, lanes, VRF, VLSU,
backend de memoria e interconexión. Los contratos de `common/` e `interface/`
requieren revisión de ambas partes; cada edición tendrá un responsable.

| Fase | VPU | CPU/Minor | Cierre de integración |
| --- | --- | --- | --- |
| 2.1 Contratos | Tipos y soporte | Captura y estados | Ciclo de vida común |
| 2.2 Anchos | Cálculo y geometría | Decode y grupos | Perfil SEW/LMUL |
| 2.3 Recursos | Colas y arbitraje | Esperas y respuestas | Progreso finito |
| 2.4 Bloques | VLSU y memoria | Orden y faults | Cierre memoria/VRF |
| 2.5 Concurrencia | Contextos y reservas | Envío y retirada | Solapamiento |
| 2.6 FUs | FP32 y varias ALUs | Operaciones y flags | Cálculo y efectos |
| 2.7 Red | Crossbar y slides | Traducción de slides | Transporte y writeback |
| 2.8 Cierre | Recursos y métricas | Estado CPU | Evidencia y límites |

## Acciones en CPU y Minor

### Fase 2.1. Contratos y capacidades

**Puntos de trabajo:** `decode.cc`, `vector_types.hh`, `vector_state.hh`,
`execute.hh/.cc` y los mensajes de `interface/`. Los contratos ya integrados
se consultan en la [entrega de fase 1][fase1]; no se plantea rehacerlos.

1. Mantener en CPU la identificación de la instrucción y su traducción a
   una operación semántica. Entregar a la VPU referencias y valores mediante
   `VectorCommand`, sin trasladar `StaticInst` ni `DynInst` a sus mensajes.
2. Conservar la distinción entre configuración RVV ejecutada en CPU,
   instrucción enviada a VPU y camino nativo de ejecución. Una instrucción
   enviada no debe ejecutarse además por el camino nativo.
3. Capturar configuración y operandos cuando corresponda construir el
   comando. En gem5, `vl` y `vtype` se obtienen del estado RISC-V utilizado
   por `captureVectorConfig()`; no de una copia de CSR desactualizada.
4. Mantener un `MinorVectorState` que asocie instrucción, `CommandKey`,
   comando pendiente, token de reserva y finalización. Conservar el
   seguimiento hasta retirar la instrucción o procesar su excepción.
5. Distinguir los resultados de admisión: `Stall` permite reintento,
   `Rejected` exige procesar el rechazo y `Granted` entrega una reserva que
   se consume una sola vez. `accepted` no significa instrucción terminada.
6. Coordinar con la interfaz el cierre durante drain: un intento sin reserva
   no tiene las mismas obligaciones que un token concedido o un comando ya
   enviado. No perder obligaciones al detener la admisión de trabajo nuevo.

**Resultado esperado:** poder seguir una instrucción desde su traducción
hasta su retirada, identificando qué estado conserva Minor en cada paso.
El contexto FP preparado no habilita por sí solo nuevas instrucciones FP.

### Fase 2.2. Generalización de anchos y grupos

**Puntos de trabajo:** traducción de instrucciones, `buildVectorCommand()`,
`captureVectorConfig()` y referencias de operandos. La matriz exacta está
en la [entrega de fase 1][fase1] y en el [diseño][diseno].

1. Revisar las rutas de suma `.vv/.vx` y cargas/stores unit-stride para
   8/16/32/64 bits. Localizar supuestos de cuatro bytes o valores de 32 bits
   en la construcción de comandos y sustituirlos donde limiten el perfil.
2. Expresar correctamente el comienzo y tamaño de cada grupo arquitectónico.
   Incorporar primero `m1/m2/m4/m8`, después `mf2` y finalmente `mf4/mf8` en
   las combinaciones acordadas. Un grupo fraccional utiliza parte de un
   registro contenedor; no se representa redondeando LMUL a cero registros.
3. Conservar `vill`, `vl`, `vstart`, SEW, LMUL y políticas tail/máscara en el
   comando. En memoria, conservar base, dirección de acceso y EEW; durante
   este hito los accesos admitidos mantienen EEW=SEW.
4. Para `.vx`, leer una sola vez el registro escalar y normalizarlo desde
   XLEN al ancho de la operación mediante el adaptador compartido. Para
   memoria, capturar una sola vez la dirección base.
5. Guardar el comando construido. Ante `Stall`, volver a solicitar admisión
   con el mismo descriptor y `CommandKey`: no releer registros, recapturar
   configuración ni asignar otra identidad.
6. Registrar por separado decode disponible, comando representable y
   combinación admitida por `VectorSupport`. Una combinación fuera del
   perfil no debe llegar a producir efectos en la VPU.

**Ejemplo:** una suma captura un escalar de valor 5 y recibe `Stall` durante
varios ciclos. Todos sus intentos conservan ese valor, sus registros, su
configuración y su identidad hasta obtener reserva o rechazo permanente.

**Resultado esperado:** suma, carga y store recorren el camino completo para
las combinaciones del perfil, todavía con una instrucción activa. La mera
existencia de tipos de 64 bits no acredita ejecución en ese ancho.

### Fase 2.3. Esperas, admisión y reconocimiento de finalización

**Puntos de trabajo:** fases de `MinorVectorState`, `commitInst()`,
`accepted()`, `completed()` y activación de Execute en `evaluate()`.

1. Conservar la instrucción pendiente cuando `requestGrant()` devuelve
   `Stall`. Mantener el comando y permitir un nuevo intento posterior;
   comprobar que el pipeline no deja de activarse mientras necesita
   reintentar la admisión.
2. Cuando se recibe `Granted`, guardar el token, preparar el estado de
   recepción y efectuar `dispatch()` una sola vez. Preparar dicho estado
   antes de llamar al receptor, porque los callbacks pueden ser síncronos.
3. Al recibir `accepted()`, marcar la aceptación y conservar la asociación
   con la instrucción. Puede liberarse la copia del descriptor según el
   contrato existente; se mantiene la identidad y el seguimiento pendiente.
4. Al recibir `completed()`, guardar la respuesta, marcar el estado como
   completado y despertar Execute. El callback no retira la instrucción
   ni aplica directamente sus efectos arquitectónicos.
5. Procesar la respuesta desde el camino de retirada. Liberar la entrada
   una sola vez y detectar respuestas desconocidas, duplicadas o recibidas
   en una fase incorrecta como errores internos del protocolo.
6. Revisar que drain e interrupciones no descarten comandos con obligaciones
   aceptadas. La ausencia de actividad local no demuestra que la VPU haya
   cerrado sus respuestas o que Minor las haya procesado.
7. Preparar la propuesta CPU de 2.5: identificar dónde separar envío y
   retirada, cómo localizar cada respuesta y qué capacidad tendrá el
   seguimiento. Esta preparación no activa concurrencia en 2.3.

**Secuencia que debe conservarse:** comando pendiente, reserva, envío,
aceptación, finalización y procesamiento en retirada. Las esperas entre
esos pasos pueden crecer al temporizar recursos; no cambian su significado.

**Resultado esperado:** la CPU conserva y procesa correctamente una
instrucción ante esperas de duración variable. Se mantiene una instrucción
vectorial activa durante esta fase.

### Fase 2.4. Orden con memoria escalar y tratamiento de faults

**Puntos de trabajo:** `commitInst()`, interacción con LSQ/store buffer,
`hasPendingVpuStoreBefore()`, emisión anticipada de memoria y finalizaciones.

1. Antes de enviar una carga/store vectorial, comprobar que han terminado
   y se han retirado las instrucciones anteriores y que han cerrado los
   accesos escalares pendientes relevantes para la barrera. Revisar tanto
   LSQ como store buffer: un store retirado puede seguir pendiente de envío.
2. Revisar los caminos de emisión anticipada de memoria escalar para que
   respeten esa política. No limitar la comprobación al camino normal de
   commit; un acceso adelantado también puede alterar el orden observado.
3. Mantener la memoria vectorial como barrera: no despachar trabajo posterior
   hasta procesar su éxito o fault. Esta restricción se conserva al introducir
   la ventana aritmética de 2.5.
4. Recibir una finalización por comando. Minor no lleva el seguimiento de
   cada bloque o paquete físico ni inicia el siguiente bloque de la VLSU.
5. Guardar la respuesta por identidad y procesarla en retirada. En éxito,
   aplicar el `finalVstart=0` del contrato y continuar la retirada normal.
6. Ante `MemoryFault`, aplicar el `finalVstart` recibido y entregar el objeto
   `Fault` al tratamiento de excepciones de gem5. No marcar la instrucción
   como retirada con éxito ni liberar trabajo posterior como si no hubiera
   fallado. El comportamiento concreto de la excepción depende del entorno
   SE; no se presupone una rutina de sistema operativo invitado.
7. Conservar el seguimiento hasta cerrar el tratamiento de éxito o fault.
   La VPU debe entregar una respuesta terminal después de cerrar sus
   obligaciones; la CPU debe procesarla antes de declarar terminado drain.

**Ejemplo:** si la VPU devuelve un fault en el elemento 7, Minor aplica
`finalVstart=7`. Es un índice vectorial, no el número de bloque ni de paquete.
Localizar el elemento causante dentro del bloque corresponde a VLSU/backend.

**Resultado esperado:** agrupar memoria en bloques no cambia la barrera ni
el tratamiento arquitectónico de la respuesta. Se conserva la identidad
del comando hasta completar su procesamiento en CPU.

### Fase 2.5. Despacho y retirada de varias instrucciones

**Puntos de trabajo:** `execute.hh/.cc`, `vpuOffloadStates`, callbacks,
seguimiento del pipeline y coordinación con `CpuVectorInterface`.

1. Separar el cursor de despacho vectorial de la cabeza de retirada. Permitir
   enviar una segunda instrucción aritmética mientras la primera espera su
   finalización, manteniendo envío y retirada en orden de programa.
2. Abrir la ventana sólo después de cerrar instrucciones anteriores y
   resolver el control que podría invalidarla. Establecer una capacidad
   finita de seguimiento CPU, independiente de los créditos internos VPU.
3. Mantener una entrada por instrucción pendiente. Capturar sus operandos y
   configuración una vez, y conservar su `CommandKey` hasta retirada.
4. Adaptar `accepted()` y `completed()` para localizar la entrada por clave.
   El uso exclusivo de `vpuOffloadStates.front()` en esos callbacks sólo
   sirve mientras la respuesta pertenece necesariamente a la primera.
5. Guardar una finalización aunque corresponda a una instrucción joven.
   Aplicar `vstart`, flags y demás efectos CPU únicamente cuando le toque
   retirarse; no volver a ejecutar su semántica por el camino nativo.
6. Detener el despacho ante memoria, instrucciones escalares, configuración,
   CSRs, control y operaciones rechazadas. Si se rechaza una instrucción
   joven, cerrar las anteriores antes de procesar su excepción local y no
   enviar posteriores. Ejecutar aisladas las que tengan `vstart` no nulo.
7. Revisar activación del pipeline, descarte, interrupciones y drain para
   todas las entradas con obligaciones. Una revisión que sólo inspeccione
   la cabeza no basta cuando existen varios comandos enviados.
8. Registrar ocupación, espera de admisión, finalizaciones retenidas y
   bloqueos por barrera o capacidad CPU, distinguiéndolos de esperas VPU.

**Ejemplo:** se envían A y B; termina B primero y su respuesta se guarda;
termina A; se retira A y después B. La recepción de B no autoriza aplicar
su estado CPU antes de A.

**Resultado esperado:** dos instrucciones independientes del programa
solapan ejecución. La ventana sigue limitada a aritmética vectorial
consecutiva; el progreso escalar general pertenece al hito 3.

### Fase 2.6. Nuevas operaciones, contexto FP y flags

**Puntos de trabajo:** traducción semántica, construcción del comando,
validación de estado FP y procesamiento de `VectorCompletion` en retirada.

1. Incorporar las rutas CPU de `vfadd.vv`, `vfmul.vv` y `vfdiv.vv` FP32,
   y de `vmul`, `vdiv`, `vdivu`, `vrem` y `vremu` en formas `.vv/.vx`
   para los anchos enteros acordados. Consultar la matriz del [hito][hito2].
2. Comprobar legalidad, estado arquitectónico y soporte de la combinación
   antes de admitir efectos. Distinguir la operación con y sin signo en
   el descriptor que recibe la VPU.
3. Capturar el redondeo FP al crear el comando y conservarlo durante las
   esperas. No sustituir silenciosamente un modo inválido por otro válido.
4. Recibir `fpFlags` agregados por comando. Guardarlos con la finalización y
   acumularlos en `fflags` al retirar, preservando los flags anteriores.
5. Tratar los accesos a `frm`, `fflags` y `fcsr` como límites de la ventana
   para que no observen o cambien contexto en un punto incorrecto.
6. Conservar el seguimiento de varias operaciones aunque tengan tiempos
   distintos. El número de ALUs por lane es configuración interna VPU;
   Minor no selecciona qué instancia ejecuta cada fragmento.

**Ejemplo:** una operación FP termina antes que otra instrucción anterior.
Su respuesta queda retenida y sus flags se aplican cuando alcance retirada.

**Resultado esperado:** cada operación tiene ruta CPU completa y sus efectos
se aplican una sola vez en orden. No se anuncia FP16, FP64, FMA ni formas
`.vf` por haber preparado mensajes o adaptadores internos.

### Fase 2.7. Traducción de slides y seguimiento

**Puntos de trabajo:** decode/traducción, descriptores de operandos,
construcción de comandos y seguimiento de retirada.

1. Traducir `vslide1up.vx` y `vslide1down.vx` a sus operaciones semánticas,
   conservando fuente, destino, escalar y configuración en 8/16/32/64 bits.
2. Capturar y normalizar el escalar una sola vez. Aplicar con la autoridad
   compartida las restricciones de grupos y solapamiento de cada operación;
   no tratar up y down como operaciones con idénticas restricciones.
3. Rechazar las formas predicadas fuera del perfil de esta fase, en lugar
   de ejecutarlas ignorando la máscara. Mantener `vstart` y políticas tail.
4. Utilizar el mismo protocolo de reserva, aceptación, finalización y
   retirada. Minor no crea paquetes entre lanes ni espera sus entregas
   individuales: espera la finalización del comando completo.

**Resultado esperado:** ambas slides pueden ejecutarse desde un programa
con sus restricciones y estado arquitectónico correctos. La existencia del
crossbar por sí sola no acredita soporte de estas instrucciones.

### Fase 2.8. Consolidación de la integración CPU

**Puntos de trabajo:** matriz de soporte, documentación de integración,
estadísticas de Minor y revisión del ciclo de vida completo.

1. Relacionar cada operación, forma, ancho y configuración con su ruta de
   decode, construcción, admisión y retirada. Marcar lo implementado,
   integrado y comprobado por separado de pendientes o candidatos.
2. Revisar que ninguna finalización retenida se pierda al cerrar una ventana,
   procesar una barrera, tratar una excepción o completar drain.
3. Consolidar métricas que expliquen por qué Minor no envía más trabajo:
   capacidad, barrera, falta de admisión o espera de retirada. Acordar su
   relación con los contadores VPU para interpretar el recorrido completo.
4. Identificar los cambios futuros para progreso escalar, retornos escalares,
   máscaras y memoria concurrente según el [plan de evolución][plan].
   Documentar los límites actuales sin activar esas capacidades.

**Resultado esperado:** la matriz distingue soporte interno de instrucciones
utilizables desde Minor, y existe evidencia acordada del ciclo CPU--VPU--CPU.

## Acciones en la VPU

### Fase 2.1. Contratos, soporte y construcción

**Puntos de trabajo:** `common/`, `interface/`, `VectorEngine`,
`configuration.py` y parámetros. La [entrega de fase 1][fase1] documenta
los contratos integrados que deben conservar las ampliaciones.

1. Mantener separados operación semántica, referencias arquitectónicas,
   formatos, anchos, rangos y bytes transportados. ALU/FPU reciben bundles
   resueltos y no decodifican instrucciones CPU.
2. Usar `OperationTraits` y `VectorSupport` como autoridad compartida de
   soporte, evitando listas contradictorias en frontend, lanes o memoria.
   Distinguir descriptor inválido, combinación no soportada y falta temporal
   de recursos. La matriz objetivo no equivale a soporte activo.
3. Conservar la identidad desde comando hasta tarea, fragmento, acceso al
   VRF y petición de memoria. Cada respuesta debe identificar su solicitud;
   dos contadores de recursos distintos no se sustituyen entre sí.
4. Definir quién conserva cada buffer y crédito hasta su cierre. Instalar
   el estado receptor antes de emitir solicitudes; una respuesta puede
   llegar dentro de la llamada que las envía.
5. Mantener los contratos de bytes, contexto FP, bloques e interconexión
   preparados sin habilitar recursos u operaciones de fases posteriores.
6. Validar la construcción y entregar parámetros propios a cada módulo.
   Conservar una autoridad de mapeo y una configuración coherente con CPU
   para VLEN, ELEN y orden de bytes.

**Resultado esperado:** cada mensaje tiene un receptor y cada recurso un
propietario y momento de liberación. Los tipos preparados no se presentan
como ejecución comprobada de todas sus combinaciones.

### Fase 2.2. Geometría, datos y cálculo por ancho

**Puntos de trabajo:** `ElementCodec`, `AddressMapper`, tareas, distribuidor,
`AraLane`, ALU, VRF, VLSU y backend de memoria.

1. Transportar buffers de bytes con ancho explícito y eliminar supuestos
   de 32 bits en suma, lectura, escritura y memoria. Centralizar extracción
   y empaquetado sin depender del orden de bytes o aritmética del host.
2. Hacer que el mapper resuelva bytes a lane, banco y posición. Mantener
   elementos íntegros en palabra según la geometría del hito; cambiar SEW
   reinterpreta los bytes y no redistribuye físicamente el VRF.
3. Fragmentar el trabajo manteniendo índices globales, offsets y cobertura
   exacta del rango. Resolver cruces entre palabras y registros mediante
   accesos correctos, sin leer o escribir bytes ajenos al trabajo.
4. Ampliar suma y memoria por elemento a los cuatro anchos. En cálculo,
   aplicar el adaptador del ancho activo; en memoria, mantener tamaño y
   posición de cada elemento hasta cerrar su transferencia con el VRF.
5. Incorporar grupos enteros y fraccionales en el orden de la fase 2.2.
   Validar alineación, capacidad y límites; preservar prestart y tail,
   incluida la parte no utilizada del registro contenedor fraccionario.
6. Tratar rangos vacíos sin emitir tareas ni accesos, conservando el cierre
   único del comando. Actualizar soporte activo sólo para combinaciones
   realmente implementadas.

**Ejemplo:** un grupo `m4` puede requerir accesos a varios registros y lanes.
Los fragmentos conservan la posición de cada byte en el grupo; ninguna lane
declara por sí sola finalizada la instrucción completa.

**Resultado esperado:** suma, carga y store cubren la matriz acordada sin
límites ocultos de transporte. Se mantiene una instrucción activa y memoria
por elemento; los bloques agrupados se incorporan en 2.4.

### Fase 2.3. Recursos finitos, eventos y arbitraje

**Puntos de trabajo:** `VectorEngine`, `AraLane`, `LaneRegisterFile`,
`VectorRegBank`, ALU y solicitantes de VLSU.

1. Exponer capacidad y servicio de bancos, puertos, buffers y unidades.
   Diferenciar latencia, intervalo de iniciación y número de entradas; cada
   parámetro debe tener unidades y restricciones de construcción claras.
2. Arbitrar en `LaneRegisterFile` las peticiones de operandos, writeback y
   VLSU. Aplicar la política de la fase, incluido orden de conflictos sobre
   los mismos bytes, sin retener en una cola oculta solicitudes denegadas.
3. Ante `Retry`, conservar petición, datos e identidad en el emisor y
   reintentar según el contrato. El rechazo temporal no crea otra petición
   lógica ni permite que el receptor produzca una respuesta terminal.
4. Reservar espacio de recepción antes de solicitar lecturas y espacio para
   resultados antes de ejecutar. No aceptar trabajo suponiendo que habrá
   sitio para su respuesta cuando llegue.
5. Programar eventos de servicio y finalización. Conservar operandos y
   resultados durante el bloqueo, y no sobrescribir un buffer ocupado con
   el siguiente fragmento.
6. Diferenciar concesión de acceso, recepción del dato y `WriteAck`. Un
   crédito de puerto concedido no demuestra que los bytes ya estén escritos.
7. Incluir reservas, eventos, respuestas y resultados retenidos en drain.
   Contar accesos y esperas por banco, puerto y ancho para explicar presión
   sobre el VRF.

**Ejemplo:** dos solicitantes necesitan un puerto ocupado. El no concedido
retiene su acceso y lo reintenta; el concedido tiene garantizada la recepción
de su respuesta y libera sus recursos en el punto acordado.

**Resultado esperado:** con capacidades pequeñas se conserva el progreso
sin pérdidas ni duplicados. Esta fase mantiene una instrucción activa;
temporizar recursos no introduce por sí solo concurrencia de instrucciones.

### Fase 2.4. Formación y cierre de bloques de memoria

**Puntos de trabajo:** `AraVLSU`, `VectorMemoryBackend`, mensajes de memoria,
buffers, mapper y accesos al VRF.

1. En VLSU, recorrer el rango de la instrucción y formar bloques contiguos
   sin huecos ni solapamientos. Recortar por línea, página, capacidad y final
   parcial; revisar dirección, desalineación y casos no agrupables.
2. Mantener por bloque dirección, primer elemento, cantidad de elementos,
   tamaño, ancho y `(TaskKey, requestId)`. No confundir índice de elemento
   con offset de bytes, número de bloque o fragmento de paquete.
3. En stores, reunir los bytes desde el VRF y correlacionar respuestas por
   acceso y offset. Enviar memoria cuando el buffer esté completo y
   conservarlo durante los reintentos.
4. En cargas, recibir los datos del bloque, repartirlos mediante el mapper
   y esperar todos los `WriteAck`. No iniciar el siguiente bloque mientras
   queden escrituras del actual pendientes.
5. En el backend, reservar retorno, traducir y producir paquetes conservando
   uno físico en vuelo. Un retry de `sendTimingReq` espera `recvReqRetry`;
   no reenvía un paquete ya aceptado porque tarde su respuesta.
6. Distinguir rechazo previo a aceptación y respuesta terminal de una
   petición aceptada. Cada petición aceptada termina una sola vez y mantiene
   su correlación con el bloque original.
7. Definir qué accesos pueden agruparse manteniendo localización precisa de
   fallos. Elegir el recorrido por elemento desde el principio cuando sea
   necesario; no reemitir stores para averiguar qué elemento falló.
8. Agregar el cierre del comando y devolver a Minor éxito o fault con causa,
   dirección e índice. Cerrar obligaciones de memoria, VRF y callbacks antes
   de comunicar la finalización; no escribir CSRs desde VLSU/backend.

**Ejemplo:** para veinte elementos de cuatro bytes, alineados y con máximo
de bloque y línea de 64 bytes, se forman bloques de 16 y 4 elementos si los
demás límites lo permiten. El segundo espera el cierre completo del primero.

**Resultado esperado:** un comando de memoria, un bloque y un paquete físico
activos, con datos conservados y cierre exacto. Contar por separado bloques,
paquetes, bytes, accesos al VRF y reintentos.

### Fase 2.5. Contextos concurrentes y dependencias

**Puntos de trabajo:** `CommandQueue`, `AraSequencer`, scoreboard vectorial,
`TaskDistributor`, `AraLane` y agregación de finalizaciones.

1. Sustituir el único contexto activo por una colección acotada. Distinguir
   comandos pendientes, activos y terminados, conservando sus créditos hasta
   el cierre definido por el contrato.
2. Mantener admisión e inicio en orden. Iniciar sólo la cabeza elegible;
   no introducir bypass de una instrucción bloqueada ni reservas contra
   sus propios operandos.
3. Reservar lecturas y escrituras de grupos arquitectónicos completos.
   Permitir lecturas compartidas y bloquear conflictos RAW, WAR y WAW según
   el scoreboard conservador del hito.
4. Mantener estado independiente por tarea y fragmento en distribuidor y
   lanes. Asociar lecturas, resultados y writebacks mediante sus claves;
   las respuestas pueden llegar en un orden distinto al de emisión.
5. Cerrar la ejecución del productor después de completar sus obligaciones
   y confirmar todos sus writebacks. Hasta ese punto, el consumidor sigue
   esperando: no hay disponibilidad parcial ni chaining.
6. Entregar una finalización por comando, aunque las instrucciones terminen
   en distinto orden. El cierre de un fragmento o un `WriteAck` individual
   no libera por sí solo las reservas de toda la instrucción.
7. Conservar memoria y contexto como barreras coordinadas con Minor.
   No introducir renombramiento, ROB o recuperación especulativa para
   ampliar silenciosamente la ventana.
8. Incluir todos los contextos y respuestas aceptadas en drain. Registrar
   ocupación y esperas por dependencia, capacidad, unidades y writeback.

**Ejemplo:** dos sumas con destinos independientes pueden avanzar juntas;
una suma que lee el destino de la anterior espera a su cierre completo.
Las lecturas y escrituras siguen compitiendo por los recursos de 2.3.

**Resultado esperado:** varias instrucciones del programa progresan con
estado independiente. El paralelismo de lanes para una sola instrucción
no satisface por sí solo este criterio.

### Fase 2.6. FP32, operaciones enteras y varias ALUs

**Puntos de trabajo:** FPU, ALU, `AraLane`, bundles/resultados, soporte,
configuración y agregación de flags.

1. Incorporar los adaptadores FP32 de suma, multiplicación y división del
   perfil. Recibir formato y redondeo explícitos; conservar patrones de bits
   y evitar que el resultado dependa del redondeo implícito del host.
2. Devolver flags con cada resultado y agregarlos por fragmento, tarea y
   comando sin incluir elementos no ejecutados. Entregarlos a Minor en la
   finalización; las unidades no modifican directamente `fflags`.
3. Incorporar multiplicación de bits bajos, división y resto con y sin signo
   en ALU para 8/16/32/64 bits y formas `.vv/.vx`. Tratar los casos extremos
   del perfil sin aritmética indefinida del host. La FPU conserva sólo FP.
4. Definir por operación latencia, intervalo de iniciación, ancho y capacidad.
   Explicitar qué recursos se comparten, especialmente en divisiones largas.
   No suponer una subunidad independiente para cada operación FP.
5. Instanciar `alus_per_lane` ALUs con ocupación propia. Repartir los bundles
   de forma determinista, cubriendo cada elemento una sola vez. Distinguir
   número de ALUs de ancho SIMD.
6. Arbitrar operandos y resultados de ALUs/FPU mediante los puertos finitos
   del VRF y el writeback compartido. Una unidad libre no implica que haya
   datos o capacidad para ejecutarla.
7. Conservar resultados bloqueados y ampliar drain a todos los eventos y
   unidades. Una división larga no debe retener créditos pertenecientes a
   otras instancias o a la otra clase de unidad.

**Ejemplo:** una división FP larga y una suma entera independiente pueden
progresar a la vez si existen operandos, puertos y buffers. Los contadores
deben explicar si la limitación procede de cálculo, lectura o writeback.

**Resultado esperado:** las operaciones del perfil tienen cálculo,
temporización y efectos completos. Variar el número de ALUs preserva los
resultados y permite explicar cambios de utilización y contención.

### Fase 2.7. Transporte modular y ejecutor de slides

**Puntos de trabajo:** `VectorInterconnect`, modelo crossbar, ejecutor de
slides que se concrete en su propuesta, lanes, distribuidor y sequencer.

1. Separar el significado de la operación de su transporte. El ejecutor
   determina qué bytes mover; la red recibe origen, destino, rango, datos
   e identidad sin interpretar opcodes o semántica de slide.
2. Resolver mediante el mapper qué accesos son locales y cuáles cruzan lanes.
   Conservar fuente, destino y asociaciones con tareas/VRF aunque las
   entregas lleguen desordenadas. Documentar los recursos de la ruta local.
3. Construir el crossbar detrás de la interfaz común, con buffers/créditos
   finitos y arbitraje por destino. Separar latencia de propagación, servicio
   en bytes/ciclo y espera por contención.
4. Reservar recepción antes de aceptar transferencias. Ante `Retry`, el
   emisor conserva los mismos bytes y `TransferKey`; una transferencia
   aceptada tiene entrega única y no se vuelve a enviar por esperar.
5. Incorporar ambas slides del perfil sin predicación, incluyendo rangos
   vacíos, extremos y conservación de fuentes en solapamientos legales.
   Mantener las restricciones específicas de up y down antes de efectos.
6. Esperar las confirmaciones del VRF de destino antes de cerrar tareas y
   comando. Recibir un paquete no equivale a completar su writeback.
7. Contabilizar tráfico por enlace, esperas y ocupación con unidades comunes.
   Incluir paquetes, recepciones y writebacks pendientes en drain; mantener
   consumidores independientes de rutas y árbitros internos del crossbar.

**Ejemplo:** dos transferencias hacia un destino ocupado compiten por su
servicio. El envío denegado queda en su emisor; el aceptado conserva espacio
de recepción y su tarea espera después el writeback correspondiente.

**Resultado esperado:** ambas slides completan el recorrido desde programa,
con tráfico observado cuando cruzan lanes. Se puede sustituir el modelo de
red conservando sus contratos; no se exige implementar otra topología.

### Fase 2.8. Consolidación de recursos y entrega al hito 3

**Puntos de trabajo:** matrices, fichas de operación, configuración,
estadísticas, documentación y revisión de los módulos anteriores.

1. Relacionar cada combinación anunciada con soporte, unidad ejecutora,
   recursos, ruta CPU y evidencia. Diferenciar capacidad representable,
   implementación, integración y comprobación.
2. Revisar propiedad y liberación de comandos, tareas, accesos, paquetes,
   buffers y créditos. Seguir éxito, retry, rechazo y fault hasta su cierre
   sin dejar respuestas o eventos fuera de la contabilidad de drain.
3. Consolidar métricas de dependencias, FUs, VRF, memoria y red. Evitar contar
   un reintento como petición nueva o confundir paquete entregado con bytes
   aplicados al VRF. Declarar las unidades y reglas de agregación.
4. Mantener el procedimiento de extensión del [diseño][diseno]: semántica,
   operandos, recursos, efectos, archivos y evidencia. Una familia que cambia
   contratos necesita una propuesta distinta de añadir un adaptador local.
5. Identificar qué contratos deben ampliarse para rangos, predicación,
   resultados escalares, colectivos, memoria concurrente y clústeres según
   el [plan][plan]. Documentar responsables y puntos de sustitución sin
   implementar anticipadamente esas capacidades.
6. Relacionar los criterios de cierre con la evidencia acordada del
   [plan de benchmarks][benchmarks]. Declarar límites y perfiles sin
   calibrar; ejecutar kernels no acredita equivalencia de ciclos con Ara.

**Resultado esperado:** la entrega explica qué funciona, con qué límites y
qué evidencia lo respalda. El hito 3 recibe contratos y recursos trazables,
sin reabrir el baseline cerrado ni dar por resueltas sus ampliaciones.

## Referencias

- [Diseño detallado del hito 2][diseno]: secciones 7, 10, 11, 13 y 15 para
  despacho, memoria, red, extensión, fases y criterios de cierre.
- [README y matriz del hito 2][hito2] y [entrega de fase 1][fase1]: alcance
  de operaciones, geometría y distinción entre soporte activo y objetivo.
- [Interfaces del baseline][interfaces]: responsabilidades iniciales;
  sus ampliaciones se consultan en los documentos del hito 2.
- [Plan de evolución][plan], [resumen][resumen] y [benchmarks][benchmarks]:
  dependencias, continuidad y evidencia que debe acordarse.
- [Manual oficial RVV 1.0, edición 20240411][rvv]: referencia normativa.
  Secciones de consulta para esta guía: *Vector Register Grouping*,
  *Vector Operands*, *Vector Start Index CSR*, *Vector Slide1up* y
  *Vector Slide1down*. El [diseño][diseno] recoge las referencias de las
  demás operaciones. Sus propuestas concretarán formas, restricciones y
  casos; esta guía no sustituye su revisión normativa.

[diseno]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md>
[hito2]: README.md
[fase1]: <Fase 1 - Contratos y capacidades.md>
[objetivos]: <../Objetivos a medio y largo plazo.md>
[resumen]: <../Resumen de hitos - Objetivos y alcance.md>
[plan]: <../Plan de evolución - Hitos 2 a 5.md>
[benchmarks]: <../Plan de benchmarks.md>
[interfaces]: <../hito1/Interfaces propuestas.md>
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
