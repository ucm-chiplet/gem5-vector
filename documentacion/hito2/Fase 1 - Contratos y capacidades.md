# Fase 1 del hito 2: contratos y capacidades

Implementación autorizada por el usuario el 26 de octubre de 2026, sobre
`5fc5c12f1047`. Esta entrega atiende O2 (modelo con recursos finitos) y O6
(contratos y resultados trazables). Prepara las comparaciones de O3; no
implementa modelos tecnológicos, IME ni organizaciones alternativas.

El baseline del hito 1 permanece cerrado. Esta fase adapta representación,
validación, construcción y cierre; no añade instrucciones ni acredita los
resultados funcionales o temporales de las fases siguientes.

## Semántica, capacidades y datos

`OperationTraits` describe las operaciones implementadas: clase de ejecución,
formato, anchos, formas, contexto FP, faults posibles y admisibilidad en la
frontera CPU--VPU. Esto último no acredita la ruta de decode de cada forma
y ancho, que se registra por separado. `VectorSupport` consulta esos
metadatos y separa:

| Resultado | Respuesta | Efecto |
| --- | --- | --- |
| Descriptor inválido o configuración arquitectónica inválida | `Rejected` con motivo `Invalid*` | Sin reserva. |
| Operación/configuración fuera del soporte activo | `Rejected` con motivo `Unsupported*` | Sin reserva. |
| Falta temporal de capacidad | `Stall` en grants; `Retry` en enlaces internos | Sin transferencia ni efectos. |

La frontera valida estructura e identidad; admisión añade configuración,
capacidad efectiva y soporte. La disponibilidad se consulta después de los
rechazos permanentes. Minor conserva la construcción de la excepción local.
Las identidades desconocidas, respuestas duplicadas y cambios de descriptor
tras reservar son errores internos, no faults del programa.

`describeOperands` produce una vista derivada de roles, acceso, formato,
EEW y EMUL. No almacena una copia mutable paralela a los operandos del
comando. El store identifica una fuente; la carga, un destino. Los roles
de acumulador/predicado y destinos escalares son puntos de extensión, sin
operaciones activas que los utilicen. Ampliar las fuentes de una operación
requiere ampliar su descriptor y validación; no basta añadir un rol al enum.

Los mensajes de cálculo transportan `ByteBuffer`. Su tamaño es
`elementCount * elementBytes`; se conserva el orden creciente de elementos
y offsets de bytes. El orden de bytes del objetivo viaja con el bundle y el
resultado. `ElementCodec` interpreta los patrones sin depender del host.
Sus adaptadores representan 8/16/32/64 bits; esa capacidad de representación
no habilita operaciones en todos esos anchos. La ALU y la FPU conservan
sus capacidades anteriores.

Minor captura una vez configuración, referencias y valores escalares. La
normalización `.vx` se realiza al construir el comando. Ni `Stall` ni
`Retry` vuelven a consultar registros CPU. El snapshot incluye `vill`,
`vl`, `vstart`, SEW, LMUL y políticas tail/máscara.

## Matriz objetivo y soporte activo

El perfil objetivo de suma `.vv/.vx` y memoria unit-stride con EEW=SEW,
para ELEN=64, es:

| SEW | m1 | m2 | m4 | m8 | mf2 | mf4 | mf8 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 8 | Objetivo | Objetivo | Objetivo | Objetivo | Objetivo | Objetivo | Objetivo |
| 16 | Objetivo | Objetivo | Objetivo | Objetivo | Objetivo | Objetivo | Fuera |
| 32 | Objetivo | Objetivo | Objetivo | Objetivo | Objetivo | Fuera | Fuera |
| 64 | Objetivo | Objetivo | Objetivo | Objetivo | Fuera | Fuera | Fuera |

`inTargetProfile` expresa esta matriz, parametrizada por ELEN. «Fuera» no
declara universalmente ilegal una combinación RVV. Para admitir trabajo
deben cumplirse además soporte implementado, habilitación, geometría,
VLMAX, alineación de grupos y restricciones de la operación. El grupo
fraccional sigue usando un registro contenedor; no crea nombres nuevos.

Se conserva prestart y tail, también con `ta`, y se capturan `mu/ma` sin
ejecutar predicación. Con `vstart >= vl` no se emiten tareas ni escrituras;
el comando conserva aceptación y finalización únicas. Los solapamientos
de igual ancho y los límites de grupos se validan antes de efectos.

## Propiedad, identidad y cierre

```text
CommandKey = (contextId, commandId)
TaskKey = (CommandKey, taskId)
LaneFragmentKey = (TaskKey, fragmentId)
VrfAccessKey = (TaskKey, requester, accessId)
memoria = (TaskKey, requestId)
TransferKey = (TaskKey, transferId)
```

La identidad del fragmento no sustituye a la tarea. Los accesos al VRF y
las peticiones de memoria tienen contadores diferentes. No se reutilizan
identificadores vivos y el agotamiento de un contador es un error interno.

| Transición/recurso | Propietario y obligación |
| --- | --- |
| Grant | La cola copia el comando y reserva un crédito; todavía no hay `accepted`. |
| Dispatch | Consume el token una vez y conserva el descriptor inmutable. |
| Aceptación | Confirma inserción, sin prometer inicio ni resultado. |
| Inicio | El sequencer conserva el crédito de comando y el contexto activo. |
| Resultado | La lane retiene datos y flags hasta cerrar el writeback. |
| `WriteAck` | Confirma bytes aplicados; no sólo aceptación del acceso. |
| Finalización | Tras cerrar todas las obligaciones, se entrega `completed`. |
| Cierre del callback | Se libera la cabeza/crédito y el contexto del sequencer. |
| Retirada | Minor aplica estado o fault y libera su entrada CPU. |

La interfaz CPU conserva un registro por comando concedido, verifica token,
descriptor y secuencia de callbacks y lo libera tras entregar la respuesta
terminal. El seguimiento es finito: corresponde a créditos concedidos.
Minor conserva independientemente su entrada hasta retirada.

Antes de enviar una petición, su emisor instala el estado receptor. Los
callbacks pueden ser síncronos. `Accepted` obliga a una respuesta terminal;
`Retry` conserva propiedad en el emisor y no permite respuestas. El retry
interno mantiene identidad y mensaje y se evalúa en un ciclo posterior.
El retry de `sendTimingReq` sigue esperando `recvReqRetry`.

Drain cierra grants nuevos, permite consumir tokens ya concedidos y espera
tareas, buffers, traducciones, paquetes, writebacks y callbacks. La VPU
vacía no certifica que Minor haya procesado sus finalizaciones retenidas.

## Contratos preparados

La petición de memoria distingue `elementBytes`, `elements`, `size` y
`dataRange`. Se exige:

```text
size = elementCount * elementBytes
dataRange.size = size
dataRange.offset = firstElement * elementBytes
```

No tiene una lane física única. La VLSU conserva la ubicación del elemento
en su estado privado; el mapper es su autoridad. `LoadData` conserva rango,
ancho, datos y clave hasta cerrar la escritura. El backend sólo admite
`elementCount == 1`; no se forma un bloque agrupado ni se aumenta el número
de paquetes físicos en vuelo.

`VectorCommand.fpContext` y `VectorCompletion.fpFlags` completan el contrato
FP. El contexto contiene redondeo resuelto, y los flags se agregan por
fragmento/tarea/comando. Los callbacks no escriben CSRs: Minor aplica los
efectos al retirar. La autoridad de soporte mantiene cerrado el offload FP
hasta aprobar e integrar la fase 6.

`VectorInterconnect` define capacidades, binding de receptores, envío,
recepción reservada y drain. Un paquete aceptado tiene entrega única y su
entrega no implica `WriteAck`. El productor conserva asociaciones hasta
cerrar los efectos en el VRF. Los paquetes se identifican por `TransferKey`,
sin dependencia de rutas o arbitraje. Se declaran puntos de observación de
intento, aceptación y entrega: retries no son paquetes nuevos. Contadores
por enlace y recursos temporizados se incorporarán con el modelo activo.
No se instancia red ni se permite habilitarla en fase 1.

## Configuración y mapeo

Se conservan los parámetros planos anteriores como entrada compatible.
Anchos de bundle/FU con valor cero se resuelven a la palabra de lane antes
de construir. Entradas son cantidades de contextos/bundles, anchos son
bytes, y latencias/intervalos son ciclos. La latencia funcional actual de
un evento no acredita fidelidad hardware Ara.

Las capacidades nuevas no se aceptan silenciosamente: se exige una
instrucción activa, un contexto/buffer por fuente/resultado, una ALU y una
FPU por lane, una petición pendiente por banco, un puerto RW, un elemento
de memoria activo y un paquete físico en vuelo. Las anchuras de FU/buffer
coinciden con la palabra; otras configuraciones requieren sus fases.
El máximo futuro de bloque es positivo y divisible por los tamaños de
elementos habilitados. VLEN/ELEN y orden/ancho del objetivo coinciden con CPU.
La descripción de interconexión declara máximo de paquete, entradas/bytes
por destino, bytes por ciclo de origen/destino y ciclos de tránsito. Los
anchos de red/buffer con cero se resuelven a una palabra de lane. Estos
límites se validan, pero no crean servicio ni tráfico mientras la red esté
deshabilitada.

## Referencia normativa y comprobaciones

Autoridad: RVV 1.0, manual oficial **edición 20240411**:
[capítulo vectorial][rvv]. Secciones consultadas: *Vector Register Grouping*,
*Vector Operands*, *Mapping for LMUL < 1*, *Vector Tail Agnostic and Vector
Mask Agnostic*, *Vector Start Index CSR*, *Prestart, Active, Inactive, Body,
and Tail Element Definitions*, *Vector Arithmetic Instruction encoding*,
*Vector Single-Width Integer Add and Subtract*, *Vector Load/Store Width
Encoding*, *Vector Unit-Stride Instructions* y *Vector Floating-Point
Exception Flags*. Los tiempos y la política física son decisiones del
modelo, no requisitos temporales RVV.

[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
