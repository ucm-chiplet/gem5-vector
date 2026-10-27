# Fase 2 del hito 2: anchos y grupos, entrega VPU

Implementación autorizada por el usuario el 27 de octubre de 2026, sobre
`8f4d975d0b4f`, limitada a las responsabilidades de la VPU. La estimación
de líneas de la propuesta era orientativa y no limita esta entrega.

Atiende O2 (modelo del subconjunto RVV con recursos explícitos) y O6
(capacidades y evidencia trazables), y prepara O3 al permitir variar ancho
y tamaño de grupo sin cambiar el mapeo físico. No evalúa organizaciones
alternativas ni costes CMOS 2.0.

## Alcance y estado

El backend admite suma entera `Add` con dos fuentes vectoriales o una
fuente vectorial y un escalar capturado, y carga/store unit-stride con
EEW=SEW. El perfil de ELEN=64 es:

| SEW | m1 | m2 | m4 | m8 | mf2 | mf4 | mf8 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 8 | Backend | Backend | Backend | Backend | Backend | Backend | Backend |
| 16 | Backend | Backend | Backend | Backend | Backend | Backend | Fuera |
| 32 | Backend | Backend | Backend | Backend | Backend | Fuera | Fuera |
| 64 | Backend | Backend | Backend | Backend | Fuera | Fuera | Fuera |

**Backend** significa capacidad implementada en las fuentes de la VPU,
condicionada a habilitación y geometría compatibles. No significa que se
haya ejecutado y comprobado cada combinación ni que todas sus instrucciones
tengan ruta CPU integrada. **Fuera** indica exclusión del perfil acordado;
no una declaración de ilegalidad universal en RVV.

Se conserva una instrucción activa, un contexto por lane, una ALU y una FPU
por lane, un acceso pendiente por banco con un puerto RW, una petición de
memoria por elemento y un paquete físico en vuelo. No se habilitan bloques
agrupados, concurrencia nueva, arbitraje nuevo ni transporte entre lanes.

La FPU binary16 interna conserva exclusivamente su capacidad previa;
ampliar la tabla de anchos enteros no habilita nuevos formatos FP ni su
offload. Predicación, widening, narrowing, Mul/Div/resto, slides, chaining,
clústeres e IME quedan fuera de esta entrega.

## Capacidades y geometría compartidas

`IntegerElementWidths` y las fichas de `OperationTraits` amplían suma y
memoria a 8/16/32/64 bits. `VectorSupport` sigue distinguiendo estructura,
perfil implementado, habilitación y ocupación. Un rechazo permanente se
resuelve antes de consultar capacidad; no se convierte en espera por tener
la cola llena. Las restricciones de grupos también se revisan con `vl=0`.

`groupRegisterCount()` expresa registros contenedores: 1/2/4/8 en grupos
enteros y uno en todos los fraccionales. `groupBytes()` expresa capacidad
efectiva en bytes mediante LMUL y VLEN. Ambas funciones exigen argumentos
representables; no conceden soporte ni recursos por sí mismas.

Admisión utiliza el mismo número de registros que las tareas. Sequencer,
distribuidor y VLSU utilizan la misma capacidad efectiva; `maxElements()`
la divide por el tamaño del elemento para obtener VLMAX. Las lanes revisan
además ese límite antes de acceder al VRF: caber en el registro contenedor
no permite escribir en el tail de un grupo fraccional. La ALU comprueba que
el destino corresponde al EMUL recibido.

Los grupos enteros exigen alineación natural y límites dentro de v0--v31.
Fuentes y destino de suma tienen el mismo ancho y grupo; los solapamientos
permitidos incluyen la actualización del propio grupo. La fragmentación
lee las fuentes del fragmento antes de escribir su resultado. Las partes
de otros fragmentos ocupan rangos distintos.

## Fragmentación, cálculo y conservación de bytes

El mapper conserva `word_striped`, independiente de SEW. Cada registro
contiene un número entero de franjas de lanes; por ello, un mismo offset
en distintos registros se ubica en la misma lane. Los grupos m2/m4/m8
continúan por registros consecutivos sin modificar ese reparto.

El distribuidor corta el rango en fronteras de palabra de fuentes/destino
y conserva índices globales dentro del grupo. No redondea el rango hasta
llenar una palabra. `ByteEnable`, el LRF y los bancos conservan bytes fuera
del rango, tanto al principio/final de una palabra como en la parte no
utilizada del registro contenedor fraccional.

`ElementCodec` ya representaba los cuatro anchos y se reutiliza sin cambios.
La ALU suma valores `uint64_t`: el caso de 64 bits usa el wrap definido del
tipo sin signo y los tamaños menores conservan los bits bajos al escribir.
No hay overflow con signo ni desplazamientos por 64 bits. Los datos siguen
el orden de bytes del objetivo, sin conversiones basadas en el host.

CPU entrega el escalar `.vx` ya capturado y normalizado desde XLEN. La VPU
lo replica en el bundle mediante el codec; los reintentos no recapturan ni
normalizan otra vez el operando CPU.

Se ejecuta exclusivamente el rango `[vstart, vl)`. Prestart y tail se
preservan, también cuando `ta` permite otra elección. Con `vl=0` o
`vstart>=vl`, el sequencer conserva su cierre único sin crear tareas ni
accesos. Se capturan `mu/ma`, pero las instrucciones con `vm=0` siguen
rechazadas: no se ignora silenciosamente su máscara.

## Memoria, identidades y cierre

La VLSU usa el mismo perfil y capacidad efectiva que el recorrido de suma.
Los tamaños de elemento son 1/2/4/8 bytes. Cada petición conserva índice,
offset de bytes relativo al grupo, dirección y datos hasta cerrar memoria
y VRF. `vstart` limita el primer elemento; no desplaza la base capturada.

El backend de memoria ya describe tamaños y datos en bytes, fragmenta por
línea/página y reúne los bytes de una carga antes de entregarla. Se conserva
esa implementación sin cambios: no agrupa elementos ni aumenta paquetes
en vuelo. Los fragmentos de un elemento mantienen la petición lógica y un
fault conserva el índice vectorial original. No se introduce una nueva
garantía de atomicidad de accesos desalineados.

Se mantienen `CommandKey`, `TaskKey`, `LaneFragmentKey`, `VrfAccessKey` y
`requestId`. Un `Retry` conserva mensaje e identidad en el emisor. La carga
espera `WriteAck` y el store espera el reconocimiento de memoria antes de
avanzar. Sólo después de cerrar las obligaciones llega la finalización;
VLSU y callbacks no escriben CSRs. Drain conserva esos mismos requisitos.

## Configuración y dependencia CPU

Los parámetros existentes permiten seleccionar el perfil. Se conservan
los valores predeterminados de 16/32 bits y m1. Por ejemplo, esta
descripción habilita el perfil completo de backend de ELEN=64:

```python
from m5.objects.configuration import VpuConfiguration, VrfConfiguration

configuration = VpuConfiguration(
    vrf=VrfConfiguration(vlen_bytes=32, lane_word_bytes=8),
    supported_sews=(8, 16, 32, 64),
    supported_lmuls=(0, 1, 2, 3, -1, -2, -3),
    elen_bits=64,
)
```

Se utiliza con `VectorEngine.from_configuration(configuration)` en una
integración con VLEN/ELEN, direcciones y orden de bytes coincidentes con CPU.
El ejemplo es una configuración propuesta, no una ejecución comprobada.
SEW y LMUL se intersectan con el perfil: no se admite automáticamente cada
pareja de su producto cartesiano. En ELEN=32 se aplica el perfil reducido.

La construcción comprueba que los elementos habilitados caben íntegros en
la palabra y en el límite de petición. Habilitar 64 bits con palabras de
cuatro bytes se rechaza al construir; no se reparte un elemento entre lanes.
El mapper mantiene su requisito de palabras múltiplo de cuatro bytes.

No se modifican Minor, ISA ni scripts de configuración CPU. La clasificación
de Minor existente reconoce `vadd.vv/.vx` y `vle32.v/vse32.v`; la ampliación
del decode de memoria pertenece a CPU. La representación de grupos y la
captura se conservan, sin atribuirles validación funcional de esta matriz.
Esta entrega no cierra por sí sola la fase 2.2 de extremo a extremo.

## Observabilidad

La ALU añade `elements8` y `elements64`, conservando los nombres previos
`elements16` y `elements32`. Cada contador mide elementos admitidos de su
ancho, una vez por bundle aceptado. Los reintentos no incrementan esos
contadores. Su suma corresponde a `alu.elements` en el mismo intervalo de
estadísticas; no implica que las operaciones hayan terminado con writeback.

Las métricas de memoria, fragmentación, bytes habilitados y cierres siguen
sus definiciones anteriores. No se añaden métricas temporales de recursos
todavía no implementados. Las latencias funcionales no se presentan como
calibración hardware de Ara.

## Referencia normativa

Autoridad semántica: **RVV 1.0, manual oficial edición 20240411**,
[capítulo vectorial][rvv]. Secciones consultadas:

- *Vector Register Grouping* y *Vector Operands*: capacidad, alineación,
  registros contenedores y solapamientos.
- *Mapping for LMUL < 1 / > 1*: parte útil fraccional y continuidad de grupos.
- *Vector Arithmetic Instruction encoding*, *Vector Integer Arithmetic
  Instructions* y *Vector Single-Width Integer Add and Subtract*: formas,
  escalar desde XLEN y suma modular, sin redondeo ni flags FP/saturación.
- *Vector Load/Store Width Encoding*, *Vector Load/Store Addressing Modes*
  y *Vector Unit-Stride Instructions*: EEW/EMUL y datos en memoria.
- *Vector Start Index CSR*, *Prestart, Active, Inactive, Body, and Tail
  Element Definitions*, *Vector Tail Agnostic and Vector Mask Agnostic* y
  *Vector Masking*: rango ejecutable, conservación y exclusión de predicación.

El subconjunto EEW=SEW y el perfil SEW/LMUL son decisiones del proyecto.
El manual no prescribe la topología física ni los tiempos del modelo.

## Comprobaciones y pendientes

Comprobaciones realizadas:

- Revisión de fuentes: representación de los cuatro anchos, captura escalar
  existente, límites de grupos, byte enables y cierre de memoria/VRF. Se
  conservan `ElementCodec`, LRF, bancos y backend de memoria sin cambios de
  implementación; sus recorridos ya operaban sobre bytes y tamaños explícitos.
- `git diff --check`: correcto. Formato C++ de los doce archivos modificados
  comprobado con `clang-format --dry-run --Werror`: correcto.
- Black/isort para `VectorEngine.py` y `configuration.py`: correctos. Black
  se comprobó por archivo para evitar el bloqueo del proceso multiproceso
  en este entorno. Sintaxis de ambos con `python3 -m py_compile`: correcta.
- SCons con 19 objetivos explícitos bajo `build/RISCV/`: capacidades, codec,
  cola, sequencer, distribuidor, frontera, lane, ALU/FPU, mapper/política,
  LRF/bancos, VLSU/backend, propietario y Python embebido. Compilación correcta;
  incluye regeneración de parámetros del SimObject. `element_codec.o` estaba
  actualizado. Se dirigió la caché de compilación a `/tmp/vpu-ccache`.
- SCons detecta GCC 15.2.1 y avisa de que está fuera de su rango declarado.
  Esta compilación no acredita soporte oficial de ese compilador.

No se enlazó el simulador completo ni se crearon o ejecutaron pruebas o
simulaciones. El enlace del auxiliar `gem5py_m5` pertenece a la generación
de parámetros, no acredita una ejecución funcional de la VPU.

Quedan para el equipo la integración CPU requerida y la evidencia funcional
de la matriz. Casos sugeridos, sin implementar: cuatro recorridos (suma
`.vv`, suma `.vx`, carga y store), cruces de grupos m4/m8, extremos de
v0--v31, escalar negativo, overflow, cambios de SEW, rangos vacíos, prestart,
tail fraccional, rechazos previos a efectos y faults por elemento. La
validación temporal y las ampliaciones de recursos pertenecen a sus fases.

[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
