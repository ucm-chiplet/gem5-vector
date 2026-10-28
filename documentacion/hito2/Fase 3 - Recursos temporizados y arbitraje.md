# Fase 3 del hito 2: recursos temporizados y arbitraje, entrega VPU

Implementación autorizada por el usuario el 28 de octubre de 2026, sobre
`9d7de4f45811`, con el alcance de la propuesta presentada en la conversación.
La entrega atiende O2 (recursos finitos) y O6 (evidencia trazable), y prepara
la observación de demanda de O1 y las comparaciones de O3. O4/O5 requieren
calibración y escenarios tecnológicos posteriores; los tiempos presentes
son parámetros del modelo, sin calibración hardware de Ara.

## Alcance y estado de la evidencia

Se implementan recursos locales temporizados sobre los recorridos de fase 2:

- Bancos con puertos R, W y RW, capacidad, latencia e intervalo por puerto.
- Arbitraje conjunto por ciclo, con solicitudes persistentes en el emisor.
- Contextos de fragmento, buffers de operandos y reservas de resultado finitos.
- Capacidad, latencia e intervalo independientes en ALU y FPU interna.
- Writeback compartido, correlación por identidad, métricas y cierre de drain.

Se mantiene una instrucción activa. Varios fragmentos de esa instrucción
pueden avanzar en distintas fases; esto no implementa la concurrencia de
instrucciones de 2.5 ni chaining. El distribuidor conserva una tarea y agrega
sus fragmentos mediante el seguimiento ya existente.

La suma entera de 8/16/32/64 bits y el perfil SEW/LMUL de fase 2 conservan
su alcance. La suma binary16 sigue siendo una capacidad interna; esta fase
no habilita offload FP, FP32, Mul/Div/resto ni otras instrucciones.
La integración y evidencia funcional de la matriz completa de fase 2
continúan pendientes según su [entrega](<Fase 2 - Anchos y grupos.md>).
Compilar esta entrega no acredita ejecución funcional ni validación temporal.

## Frontera CPU y archivos

No se modifican Minor, ISA ni los archivos de `interface/`. Se conservan
`VectorCommand`, `VectorCompletion`, grants CPU, `dispatch`, `accepted` y
`completed`. Decodificación, captura, reintento de admisión, seguimiento de
la instrucción, retirada y aplicación de CSRs/flags/faults siguen en CPU.
La nueva concesión VRF es interna y no utiliza tokens CPU.

Las acciones CPU de la [guía de responsabilidades](
<Responsabilidades CPU y VPU.md>) conservan su estado independiente. La
entrega VPU no cierra por sí sola la fase conjunta CPU--VPU.

Se modifican, bajo `src/cpu/vector_engine/`:

| Archivos | Responsabilidad |
| --- | --- |
| `configuration.py`, `VectorEngine.py` | Parámetros por módulo. |
| `common/vpu_parameters.hh` | Validación de recursos. |
| `vector_engine.hh/.cc` | Construcción, conexión y vida de módulos. |
| `vpu/register_file/lane_register_file.hh/.cc` | Arbitraje y retornos. |
| `vpu/register_file/vector_reg_bank.hh/.cc` | Servicio de bancos. |
| `vpu/lanes/ara_lane.hh/.cc` | Fragmentos, buffers y writeback. |
| `vpu/lanes/alu.hh/.cc`, `fpu.hh/.cc` | Pipeline de FUs existentes. |
| `vpu/vlsu/ara_vlsu.hh/.cc` | Acceso persistente al VRF. |
| Nuevo `vpu/register_file/vrf_port.hh` | Contrato interno y eventos. |

También se crea este documento y se actualiza el README del hito.
No hay fuentes nuevas que registrar en SCons: el archivo nuevo es un header.

## Presentación, concesión y respuesta VRF

`VrfRequestPort` enlaza un productor con el LRF mediante `peek()` y
`granted(key)`. Cada contexto de lane expone tres entradas fijas: LHS, RHS y
destino. Cada LRF tiene además una entrada de la VLSU, que sólo presenta el
elemento cuya ubicación corresponde a esa slice. El número de entradas es
`3 * lane_task_entries + 1` por LRF; no crece con los reintentos.

1. El emisor reserva recepción, conserva petición/datos y publica una oferta.
2. `VrfOffer` indica publicación y elegibilidad desde el siguiente flanco.
3. El LRF toma una instantánea de candidatos antes de conceder accesos.
4. Antes de notificar la concesión, copia la petición y reserva su retorno.
5. El banco guarda la operación concedida y reserva su dato de lectura.
6. `ReadResponse` o `WriteAck` cierra el acceso por `VrfAccessKey`.

Una oferta sin concesión permanece en su productor, con dirección, clave,
bytes y contenido estables. No transfiere propiedad ni puede recibir una
respuesta terminal. No se duplica en una FIFO de denegaciones del LRF/banco.
Las entradas de correlación del LRF y de servicio del banco representan los
mismos accesos aceptados: no constituyen dos capacidades sumables.

Las fronteras de tareas, bundles y memoria conservan `Accepted`/`Retry`.
Los accesos VRF pasan a concesión explícita; no se redefine `Retry` para
retener una petición que el receptor hubiera rechazado.

## Arbitraje, conflictos y puertos

Cada banco tiene un cursor round-robin por puerto físico. La selección
recorre puertos dedicados R, después W y finalmente RW, para aprovechar
los dedicados antes de consumir los compartidos. Un RW sirve una lectura
o una escritura por oportunidad. Lecturas y writebacks compiten como
entradas persistentes sin prioridad permanente de lecturas.

Se aplican estas incompatibilidades a accesos aceptados y a los candidatos
del mismo ciclo:

- R/R de una fila puede coexistir con recursos suficientes.
- R/W de una fila se serializa hasta cerrar el acceso anterior.
- W/W se serializa si coinciden bytes habilitados.
- W/W de bytes disjuntos y accesos a otras filas pueden coexistir.

Un candidato tampoco adelanta a otro conflictivo anterior según
`(eligibleAt, índice de entrada)`. Ese orden evita que accesos nuevos de
la misma fila impidan indefinidamente una escritura pendiente. Los índices
son estáticos y no dependen del orden de llamadas a callbacks.
Las garantías de progreso presuponen que los eventos se atienden y los
recursos concedidos terminan; falta la evidencia temporal ejecutada.

## Tiempo de servicio y eventos

Todos los módulos utilizan el reloj del SimObject `VectorEngine`.
La conversión de ciclos a Tick comprueba que el plazo no desborde.
Los eventos locales tienen prioridades explícitas:

| Evento | Prioridad gem5 |
| --- | --- |
| Respuestas de bancos y FUs | -2 |
| Progreso de lane/VLSU | 1 |
| Arbitraje del LRF | 2 |

Una oferta creada en un ciclo no compite hasta el siguiente flanco, aunque
su productor se ejecute antes que el árbitro. El conjunto de candidatos no
cambia durante la concesión por efectos de callbacks.

Para una concesión/inicio en el ciclo `c`:

- El resultado aparece en `c + latency`.
- El mismo puerto/FU puede iniciar de nuevo desde `c + initiation_interval`.
- Capacidad y conflictos pueden retrasar ese nuevo inicio.

Un banco de latencia 4 e intervalo 1 puede mantener accesos segmentados si
hay entradas y puertos suficientes. Con una sola entrada, la capacidad
limita el servicio aunque el intervalo sea uno. La respuesta de lectura
captura los bytes al terminar el acceso; la escritura los aplica antes de
emitir `WriteAck`. Los conflictos impiden depender del orden de callbacks.

El cambio de arbitraje introduce fases explícitas; conservar los parámetros
predeterminados no promete reproducir los ciclos del recorrido anterior.

## Reservas y progreso de la lane

Los contextos se recorren por `fragmentId`, independientemente del slot
físico reutilizado. Se admiten fragmentos de una sola `TaskKey`, y los
bundles inician en orden de fragmento, como máximo uno por lane y ciclo.
Un fragmento joven listo no adelanta a otro que aún reúne sus operandos.

| Recurso | Reserva | Liberación |
| --- | --- | --- |
| Contexto | Al aceptar `LaneTask`. | Al cerrar el fragmento. |
| Buffers LHS/RHS | Pareja previa a lecturas. | Al aceptar el bundle la FU. |
| Entrada FU | Al aceptar el bundle. | Al entregar el resultado. |
| Salida lane | Antes de intentar admisión FU. | `WriteAck`. |
| Banco y retorno LRF | Al conceder el acceso. | Respuesta terminal. |

Reservar ambos operandos conjuntamente evita fragmentos que retengan cada
uno una fuente y no puedan obtener la otra. `.vx` replica el escalar CPU en
su RHS reservado; no lo normaliza ni vuelve a leer registros CPU, y no pide
una lectura VRF para él.

La reserva de resultado se conserva también durante `Retry` de la FU.
Cubre ejecución, resultado retenido y writeback, con un único crédito lógico.
La FU guarda operandos/resultado dentro de su capacidad acotada; al aceptar
se liberan los bytes de operandos de la lane. Al retornar, la lane conserva
los bytes en la petición de escritura y sólo metadatos/flags en el resultado.
Las copias temporales de transporte están acotadas por sus accesos/bundles;
no se presupone almacenamiento ilimitado para callbacks.

Cada respuesta se localiza por clave. `WriteAck` sólo termina su fragmento;
otros fragmentos pueden seguir ocupando FUs o bancos. El distribuidor agrega
los cierres y el sequencer entrega una finalización por comando.
Los rangos entre fragmentos son disjuntos; en cada fragmento se reúnen las
fuentes antes de escribir. Se conservan los alias legales de igual ancho.

## Parámetros habilitados y límites conservados

| Parámetros | Unidad y condición |
| --- | --- |
| `lane_task_entries` | Contextos por lane; positivo. |
| `operand_buffer_entries_per_source` | Bundles por fuente; positivo. |
| `result_buffer_entries` | Salidas totales por lane; positivo. |
| `alu_capacity`, `fpu_capacity` | Bundles por FU; positivos. |
| `alu_latency`, `fpu_latency` | Ciclos hasta resultado; al menos uno. |
| `alu_initiation_interval` | Ciclos entre inicios ALU; al menos uno. |
| `fpu_initiation_interval` | Ciclos entre inicios FPU; al menos uno. |
| `bank_read_ports` | Puertos R por banco. |
| `bank_write_ports` | Puertos W por banco. |
| `bank_read_write_ports` | Puertos RW por banco. |
| `bank_access_entries` | Accesos en curso por banco; positivo. |
| `bank_latency` | Ciclos hasta respuesta; al menos uno. |
| `bank_initiation_interval` | Ciclos entre accesos por puerto; mínimo uno. |

El banco debe ofrecer servicio de lectura y escritura, mediante puertos
compartidos, dedicados o su combinación. Todos comparten `bank_access_entries`.

Los productos de dimensiones y bytes se comprueban antes de construir los
recursos. Una capacidad superior a la de otro recurso puede quedar sin uso;
no implica ancho de banda adicional ni garantiza mejora.
Los valores predeterminados conservan una entrada por recurso y un banco
1RW de latencia/intervalo uno.

Se exige `max_active_commands == 1`, `alus_per_lane == 1` y anchuras de
bundle/FU iguales a `lane_word_bytes`. La palabra mantiene su geometría y
los elementos íntegros de fase 2. La red continúa deshabilitada.

## Memoria y drain

La VLSU conserva una tarea, un elemento activo y un paquete físico en vuelo.
Sus fases `OfferRead`/`OfferWrite` esperan concesión sin reenviar ni sondear
el VRF. El LRF programa las oportunidades mientras existan ofertas.
Una carga espera `WriteAck` antes de avanzar; un store reúne datos antes
de enviar memoria. La petición de memoria conserva su identidad y bytes
ante `Retry`, y el retry del puerto sigue esperando `recvReqRetry`.
No se modifica el backend ni se agrupan elementos en bloques.

Drain incluye contextos, reservas, ofertas, accesos concedidos, operaciones
FU, eventos y resultados retenidos. Una fecha futura de próxima admisión,
sin trabajo pendiente, no constituye una obligación de drain.
`VectorEngine` destruye explícitamente los LRF antes de sus productores,
porque las comprobaciones de inactividad consultan los puertos enlazados.
El vaciado VPU no acredita que Minor haya procesado sus respuestas.

## Estadísticas y unidades

Se mantienen los contadores `ModuleStats`, con ocupación actual/máxima e
integración temporal hasta dump/reset. Las siguientes métricas distinguen
servicio, esperas y fases:

| Grupo/métrica | Qué mide |
| --- | --- |
| Banco: `portReads`, `portWrites` | Concesiones por puerto físico. |
| Banco: `portReadBytes`, `portWriteBytes` | Bytes habilitados por puerto. |
| Banco: `accessSizes` | Accesos por bytes habilitados; no es SEW. |
| LRF: `grantWaitTicks` | Ticks desde publicación hasta concesión. |
| LRF: `workTicks` | Acceso-ticks desde concesión hasta respuesta. |
| LRF: `waitCycles` | Solicitud-ciclos sin concesión por banco. |
| LRF: `capacityWaitCycles` | Espera por entradas del banco. |
| LRF: `portWaitCycles` | Espera por puerto compatible disponible. |
| LRF: `conflictWaitCycles` | Espera por conflictos de fila/orden. |
| Lane: `lhsBuffers`, `rhsBuffers` | Reservas de operandos. |
| Lane: `resultBuffers` | Reservas de salida. |
| Lane: `writebacks` | Fase desde resultado recibido hasta `WriteAck`. |
| Lane: `operandWaitCycles` | Fragmento-ciclos sin buffers de operandos. |
| Lane: `resultWaitCycles` | Ciclos de cabeza sin reserva de salida. |
| FU: `capacityStalls`, `intervalStalls` | Intentos rechazados por recurso. |
| VLSU: `vrfReadsGranted`, `vrfWritesGranted` | Concesiones del VRF. |

En la FU se comprueba capacidad antes que intervalo; sus contadores de
rechazo son intentos. Las causas de espera del LRF pueden coincidir.

`writebacks` no añade otra capacidad a `resultBuffers`. Los tiempos sumados
por fragmento o solicitud pueden superar el tiempo transcurrido; no se
suman las causas coincidentes como si fueran ciclos disjuntos.
Los antiguos retries VRF de lane/VLSU se sustituyen por las esperas del LRF:
una oferta persistente no genera un intento de envío nuevo en cada ciclo.
Los contadores de elementos ALU siguen contando bundles aceptados una vez.

## Referencia normativa

Se conserva la semántica existente según RVV 1.0, manual oficial edición
20240411, [capítulo vectorial][rvv]. Secciones consultadas: *Vector Register
Grouping*, *Vector Operands*, *Vector Start Index CSR*, *Prestart, Active,
Inactive, Body, and Tail Element Definitions*, *Vector Tail Agnostic and
Vector Mask Agnostic*, *Vector Single-Width Integer Add and Subtract*,
*Vector Unit-Stride Instructions* y *Vector Floating-Point Exception Flags*.

Se preservan prestart y tail, incluido el contenedor fraccional y los
rangos vacíos; las formas predicadas siguen fuera del soporte activo.
Temporizar recursos no modifica anchos, codificaciones, redondeo, flags,
faults ni el perfil SEW/LMUL, y no amplía el repertorio admitido.

## Comprobaciones y pendientes

Comprobaciones realizadas:

- Revisión de fuentes: reservas, orden de concesión/inicio, correlación por
  claves, vida de productores y cierre después de `WriteAck`. No equivale a
  ejecutar los casos funcionales o temporales.
- `git diff --check`: correcto.
- Formato C++ de los 16 archivos modificados/nuevo mediante
  `clang-format --dry-run --Werror`: correcto.
- Black por archivo e isort para los dos Python modificados: correctos.
  Sintaxis con `python3 -m py_compile`: correcta; caché dirigida a `/tmp`.
- SCons con ocho objetivos explícitos bajo `build/RISCV/`: propietario,
  lane, ALU, FPU, LRF, banco, VLSU y Python embebido de `VectorEngine`:
  compilación correcta. El proceso también actualizó Python embebido y
  parámetros generados del SimObject. Caché de compilación en
  `/tmp/vpu-ccache`; log local en `/tmp/vpu-phase3-build.log`.

SCons detecta GCC 15.2.1 y avisa de que está fuera de su rango declarado.
Además aparecen avisos `-Warray-bounds` al instanciar `statistics::Vector`
en LRF/bancos, con origen señalado en `src/base/statistics.hh` y STL.
No se atribuyen a una causa confirmada ni se modifica la infraestructura de
estadísticas para ocultarlos. Queda pendiente contrastarlos con un compilador
admitido por el proyecto. La compilación obtenida no acredita soporte de
GCC 15 ni ausencia de problemas funcionales.

No se ha enlazado el simulador completo, creado pruebas o benchmarks ni
ejecutado simulaciones. El auxiliar `gem5py_m5` utilizado por la generación
de parámetros no es evidencia de ejecución de una carga VPU.

Para el equipo se sugieren, sin implementar: 1RW/1R1W/2R1W; latencia e
intervalo variados por separado; buffers pequeños y saturados; lectura de
fuentes y writeback en un mismo banco; fuentes/destino coincidentes;
retornos entre bancos en distinto orden; rangos parciales y vacíos;
drain con ofertas, accesos, resultados y respuestas pendientes.
La contención conjunta lane/VLSU queda preparada en el árbitro, pero una
instrucción activa impide afirmar solapamiento de ambas desde un programa.

Quedan fuera: tareas/comandos concurrentes, scoreboard de 2.5, bloques de
2.4, nuevas operaciones y varias ALUs de 2.6, red/slides de 2.7, predicación,
chaining, memoria concurrente y cambios de responsabilidades CPU.

[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
