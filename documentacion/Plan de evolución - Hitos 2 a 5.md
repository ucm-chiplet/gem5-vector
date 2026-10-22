# Plan de evolución: hitos 2 a 5

Estado: propuesta documental de objetivos, fases y criterios de cierre.
No describe funcionalidades ya implementadas ni autoriza cambios de código
o la creación de pruebas. Cada fase tendrá su propuesta de implementación.

La integración funcional de MinorCPU y VPU del hito 1 está cerrada. El hito 2
incorpora concurrencia aritmética, coma flotante de 32 bits (FP32) y recursos
con capacidad y tiempos explícitos. El 3 permite solapar trabajo escalar,
memoria y cálculo vectorial, utilizando resultados parciales (chaining).
El 4 añade precisión mixta y evaluación de clústeres. El 5 completa el
repertorio vectorial necesario para ejecutar micro-kernels, álgebra lineal
de BLIS y los bloques de atención seleccionados.

Estos hitos desarrollan la base de los [objetivos globales][objetivos].
El propósito es disponer de demanda y de un modelo de entrega de datos
para comparar RVV+IME, plantillas arquitectónicas y escenarios CMOS 2.0.
Cerrar el 5 no acredita IME implementada, T1/T2/T3 evaluadas ni una ventaja
3D. Las líneas de investigación se preparan en paralelo con propuestas
específicas y criterios de comparación, sin ampliar por ello el hito 2.

| Hito | Resultado principal | Límite que se resuelve después |
| --- | --- | --- |
| 2. Concurrencia aritmética, FP32 y recursos temporizados | Anchos, recursos finitos, concurrencia independiente, FPU, memoria por bloques y red modular. | Dependencias por instrucción completa; memoria serializada; sin predicación. |
| 3. Solapamiento escalar-vectorial, chaining y memoria concurrente | Predicación, resultados tipados, chaining, memoria concurrente, progreso escalar con comandos vectoriales pendientes, colectivos y preparación para clústeres. | Precisión mixta, nuevos formatos y repertorio ampliado en 4--5. |
| 4. Precisión mixta y evaluación de clústeres | FP16, BF16, FP32/FP64, conversiones, acumulación, segmentados, compactación y evaluación temporal y de clústeres. | Completar cobertura de kernels e instrucciones habituales en el 5. |
| 5. Repertorio RVV y ejecución de kernels BLIS y atención | Enteras, máscaras, reducciones, permutaciones, memoria irregular e integración de bibliotecas. | No implica completar RVV ni ejecutar cualquier aplicación. |

El repertorio de instrucciones de los hitos 2--5 parte de un análisis
preliminar. Se revisará con la caracterización de micro-kernels y cargas
reales; cada cambio se hará mediante una propuesta que actualice la matriz
de soporte. Tampoco el número de hitos es cerrado: se prevén hitos 6 y
siguientes, por definir según los resultados (véase
[continuidad](#continuidad-de-investigación-y-líneas-paralelas)).

Recorrido del documento:

- [Hito 2: Concurrencia aritmética, FP32 y recursos temporizados](#hito-2-concurrencia-aritmética-fp32-y-recursos-temporizados).
- [Hito 3: Solapamiento escalar-vectorial, chaining y memoria concurrente](#hito-3-solapamiento-escalar-vectorial-chaining-y-memoria-concurrente).
- [Hito 4: Precisión mixta y evaluación de clústeres](#hito-4-precisión-mixta-y-evaluación-de-clústeres).
- [Hito 5: Repertorio RVV y ejecución de kernels BLIS y atención](#hito-5-repertorio-rvv-y-ejecución-de-kernels-blis-y-atención).
- [Dependencias y entregas](#dependencias-y-entregas-entre-hitos).

## Cómo usar el plan

El [diseño detallado del hito 2][diseno-h2] sigue siendo la referencia de sus
contratos, módulos y ocho fases. Su [README](hito2/README.md) contiene la matriz
de instrucciones y anchos. Este documento explica la continuidad hacia los
hitos 3, 4 y 5; no traslada al 3 requisitos ya comprometidos para el 2.
Para una lectura breve, consulta el [resumen de objetivos y alcance][resumen].
El [plan de benchmarks][benchmarks] concreta la campaña acordada y sus fichas.

## Relación con los objetivos globales

| Entrega | Qué aporta al estudio VAMPIRE | Objetivos |
| --- | --- | --- |
| 2 | Recursos finitos y métricas para explicar servicio del VRF, FUs, memoria y red. | O2, O6. |
| 3 | Chaining, memoria concurrente, progreso escalar observable y fronteras de clústeres y colectivos reutilizables para futuros caminos IME. | O2, O3, O6. |
| 4 | Formatos/precisión mixta y primer experimento parametrizable de clústeres, con perfiles numéricos y temporales. | O1, O2, O3, O6. |
| 5 | Cargas y repertorio RVV seleccionados, con ROI y demanda medida para preparar ports y comparar candidatos. | O1, O2, O6. |
| Líneas coordinadas | Especificación/ports IME, candidatos T1/T3 y estudio de modo T2, calibración y DSE planar/3D. | O1--O6. |

Las interfaces deben poder ampliarse sin incrustar una red concreta o un
acceso ilimitado a todos los registros. Esto prepara opciones futuras;
no habilita anticipadamente residencias de C, cambios de modo o motores
matriciales en los recorridos activos de 2--5.

Desde el hito 2 la VPU se prepara para ser componible: módulos con
parámetros propios, mapeo y red seleccionables al construir, y métricas
comparables por recurso y por enlace. Una organización distinta debe poder
describirse en la configuración sin reescribir los módulos que no cambian.
Estos hitos dejan la preparación; no exigen implementar organizaciones o
topologías adicionales.

En cada fase se indican trabajo, responsabilidades y resultado de cierre.
Las comprobaciones descritas son sugerencias para que el equipo defina la
validación. No se crean ni ejecutan pruebas como parte de esta revisión.
La planificación no fija todavía duración ni calendario.

Los ejemplos al final de cada hito usan las [convenciones de lectura y
referencias normativas del resumen][convenciones-ejemplos]. Son fragmentos
para explicar el objetivo de ejecución, no soporte verificado ni nuevas
pruebas. El ejemplo del hito 1 está en su [README](hito1/README.md).

Las operaciones se definirán siguiendo el [manual oficial RVV][manual-rvv]
y contrastando la [especificación congelada v1.0][rvv]. Una propuesta de
implementación fijará formas, anchos, configuración legal y casos límite.
El manual determina la semántica; este plan decide la organización interna.
Para FP16 y BF16 se consultan también las secciones específicas de las
[extensiones vectoriales FP][manual-fp] y [BF16][manual-bf16], edición
20260120. Se documentará el subconjunto admitido sin anunciar una extensión
completa cuando falten operaciones o dependencias escalares.

### Políticas tail/máscara y perfiles LMUL por hito

Esta distribución atiende O2 y O6: definir el comportamiento del subconjunto
y registrar evidencia por configuración, separando soporte, candidatos y
optimización. La referencia es RVV 1.0, [manual oficial, edición
20240411][manual-rvv], secciones **Vector Tail Agnostic and Vector Mask
Agnostic**, **Vector Register Grouping**, **Mapping for LMUL < 1** y
**Vector Operands**.

| Política | Elementos de destino afectados | Garantía |
| --- | --- | --- |
| `tu` (`vta=0`) | Tail: índices desde `vl`. | Conservar su valor anterior. |
| `ta` (`vta=1`) | Tail. | Puede conservarse el valor o escribirse unos, con las reglas de la instrucción. |
| `mu` (`vma=0`) | Elementos del cuerpo desactivados por la máscara. | Conservar su valor anterior. |
| `ma` (`vma=1`) | Elementos del cuerpo desactivados por la máscara. | Puede conservarse el valor o escribirse unos, con las reglas de la instrucción. |

Admitir agnostic y aprovecharlo para ahorrar recursos son decisiones
distintas. La implementación conservadora puede preservar los valores con
`ta/ma`; no se exige una fase posterior que los sobrescriba con unos.
Cualquier optimización de tráfico, dependencias o escrituras debe definir
su mecanismo, coste y evidencia en una propuesta específica; no tiene un
hito asignado. Las posiciones anteriores a `vstart` siguen sus reglas de
preservación; con `vstart>=vl` no se habilitan escrituras de tail. Los
destinos de máscara tienen tail agnostic incluso con `tu`; se aplican
sus reglas particulares. Ninguna política agnostic
autoriza accesos de memoria o excepciones de elementos desactivados.

| Hito | Políticas y perfil exigidos en su alcance |
| --- | --- |
| 1 cerrado | Captura de `vta/vma` y conservación fuera del rango escrito; offload sin predicación y configuración de referencia `m1`. No acredita todas las variantes. |
| 2 | Respetar `tu/ta` y capturar `mu/ma` sin predicación. Fijar la matriz SEW×LMUL en 2.1 y ejecutar suma y memoria con grupos enteros y fraccionales del perfil al cerrar 2.2. Mantener y extender ese soporte en las fases posteriores. |
| 3 | Fase 3.2: integrar `mu/ma`, escrituras parciales y memoria predicada, conservando las garantías de tail y el perfil LMUL heredado. |
| 4 | Ampliar la geometría a EEW/EMUL distintos en conversiones y acumulación; mantener las políticas y cerrar las combinaciones que requieren sus casos. |
| 5 | Completar la campaña acordada de LMUL y tail para el repertorio y los kernels seleccionados, con evidencia por combinación. No aplaza el soporte comprometido en 2--4. |

Los factores `m1/m2/m4/m8` y `mf2/mf4/mf8` son **objetivos acordados de
cierre de la fase 2.2** para suma entera y memoria unit-stride, en las
combinaciones del perfil legal fijado en 2.1. Cada factor debe estar cubierto
en las combinaciones que le correspondan; no se exige el producto cartesiano
de todos los SEW y LMUL. La matriz exacta por operación, SEW y geometría se
concretará en la propuesta de implementación de 2.1. Estos factores dejan
de ser meros candidatos, sin convertirse por ello en soporte implementado.

En 2.2 se incorporan primero los grupos enteros y después `mf2`, seguido
de `mf4/mf8`, todavía con una instrucción activa. Las fases 2.3--2.5
conservan ese soporte al introducir recursos temporizados, memoria por
bloques y concurrencia. Las nuevas operaciones de 2.6--2.7 lo extienden
según su formato y combinaciones legales. Los ejemplos `m1/m2` no fijan un
límite. Un valor en el enum o una opción de configuración no acredita
soporte ejecutado y comprobado.

La matriz identificará operación, SEW/EEW, LMUL/EMUL, legalidad según RVV,
configuraciones habilitadas y evidencia. Se comprobarán SEW/ELEN, capacidad,
alineación y solapamientos; no basta que haya espacio con el VLEN elegido.
`m4` y `m8` usan grupos de cuatro y ocho registros con comienzo múltiplo
de cuatro y ocho. Los fraccionales usan la parte inicial de un registro;
el resto pertenece al tail, también bajo `tu/ta`. No crean nombres de
registro adicionales. En precisión mixta debe comprobarse el EMUL de cada
operando y su límite de ocho registros; no se admite un destino EMUL=16.

## Campaña de benchmarks acordada

La selección del equipo sustituye los ejemplos genéricos de kernels que
contenía este plan. Se organiza en tres bloques:

- **Micro-kernels RVV:** loads/stores unit-stride, strided e indexed,
  gathers, máscaras, reducciones, permutaciones, widening, LMUL,
  configuración `vsetvl` y tratamiento del tail.
- **Álgebra lineal densa:** kernels de la biblioteca [BLIS][blis]. Se fijarán
  operaciones, revisión, tipos y la ruta vectorial efectivamente usada.
- **LLM:** Compressed Sparse Attention (CSA) y Heavily Compressed Attention
  (HCA) de [DeepSeek V4 Flash][flash], como bloques de atención con entradas
  controladas. La integración de los bloques corresponde al 5; en el 4 se
  preparan perfiles numéricos, referencias y etapas que quepan en su soporte.

El documento de benchmarks detalla la distribución por hitos. Distingue
gather de registros y cargas indexed, micro-kernel RVV y kernel interno de
BLIS, y bloque de atención y modelo completo. Las dimensiones, ROI y
variantes de ejecución se concretarán antes de implementar las cargas.
Además de los ports RVV de estos hitos, la línea IME debe comparar versiones
matriciales con la misma función y datos. Su referencia y estado se
registran aparte, sin confundir la FMA vectorial con una operación de tile.

## Decisiones que se mantienen

- Minor decodifica y entrega semántica mediante `VectorCommand`. La VPU no
  vuelve a decodificar ni depende de `StaticInst` o `DynInst`.
- `vsetvli` y el estado arquitectónico RVV permanecen bajo control de CPU.
  Aceptación, ejecución, finalización y retirada son eventos distintos.
- Se conserva identidad de comando, tarea, acceso al VRF y petición de
  memoria, también cuando las respuestas llegan en distinto orden.
- La FPU sólo ejecuta coma flotante. Mul/Div/resto enteros permanecen en la
  ALU; sus perfiles temporales no se equiparan por defecto al de la suma.
- La interconexión es modular desde el hito 2. El crossbar inicial debe
  poder sustituirse conservando contratos y métricas de sus consumidores.
- El hito 3 prepara la agrupación de lanes en clústeres dentro de una única
  VPU. La ubicación física de recursos y las fronteras entre grupos no
  deben quedar incrustadas en la semántica de las instrucciones.
- Widening y narrowing quedan fuera del 2 y entran como subconjunto definido
  en el 4. Soportar varios SEW en el 2 no habilita aritmética de ancho mixto.
- No se incorpora automáticamente renombramiento físico, ROB vectorial ni
  ejecución especulativa. Cualquier necesidad de esos mecanismos requiere
  una propuesta específica; no se activa lógica en `future/` por anticipado.
- Se trabaja en modo SE. La fidelidad temporal respecto a RTL Ara requiere
  una referencia concreta; el 4 incluye comprobación y ajuste de perfiles,
  sin atribuir equivalencia de ciclos por el nombre Ara-like.

## Hito 2: Concurrencia aritmética, FP32 y recursos temporizados

### Objetivo y resultado

Ejecutar varias instrucciones aritméticas vectoriales independientes a la
vez, con enteros de 8/16/32/64 bits y coma flotante de 32 bits (FP32).
Modelar la capacidad y los tiempos de colas, bancos, unidades de cálculo y
red para explicar sus esperas y límites de servicio. Mantener un
procedimiento estable de extensión y los ocho pasos del diseño, incluido
todo su repertorio y las dos slides.

El resultado admite enteros de 8/16/32/64 bits, suma y Mul/Div/resto `.vv`
y `.vx`, memoria unit-stride con EEW=SEW, y FP32 `vfadd`, `vfmul` y `vfdiv`
`.vv`. La red ejecuta `vslide1up.vx` y `vslide1down.vx` sin predicación en
los cuatro anchos. La matriz del README concreta las formas y restricciones.

### Fase 2.1. Contratos y capacidades

Definir mensajes, operandos, geometría, formatos y estados compartidos
antes de ampliar la ejecución. `common/` e `interface/` deben distinguir
datos, roles de lectura/escritura, rangos, efectos y contexto capturado.
`VectorSupport` será la autoridad de admisión, separando ilegalidad,
falta de soporte y falta temporal de recursos.

Se documentarán también los puntos de extensión para lectura del destino,
fuentes adicionales, máscaras y resultados escalares. Esto evita asumir
que toda instrucción tiene dos fuentes y un destino vectorial homogéneo;
no habilita esas familias todavía. El cierre entrega contratos de propiedad,
identidad, aceptación, retry, finalización, retirada y drain sin ambigüedad.

La propuesta de esta fase fija la matriz inicial SEW×LMUL para los factores
acordados `m1/m2/m4/m8` y `mf2/mf4/mf8`, el tratamiento de `tu/ta` y la
captura de `mu/ma`. Identifica combinaciones exigidas, ilegales o fuera del
perfil; no elimina un factor comprometido por considerarlo un candidato.

Define también la construcción configurable de `VectorEngine`: lanes,
bancos, VLSU, backend de memoria e interconexión con parámetros propios, y
el mapeo de bytes a lanes y bancos como política seleccionable. La política
inicial es la actual; no se habilitan organizaciones nuevas.

### Fase 2.2. Generalización de anchos

Separar los bytes transportados del cálculo por elemento. `AddressMapper`,
VRF, tareas y adaptadores deben interpretar SEW/EEW y los grupos legales,
con una geometría compatible con el ancho configurado. La suma y la memoria
unit-stride deben recorrer CPU, backend y retorno en los cuatro anchos.

El cierre exige grupos `m1/m2/m4/m8` y fraccionales `mf2/mf4/mf8` en suma
y memoria para las combinaciones de la matriz de 2.1. Primero se integran
los grupos enteros y después `mf2`, seguido de `mf4/mf8`, manteniendo una
instrucción activa. Deben resolverse límites entre registros, alineación,
capacidad efectiva y conservación del tail del contenedor fraccionario.

El cierre distingue capacidad interna de soporte arquitectónico y recoge
configuraciones admitidas y rechazadas. No introduce widening ni FP64.
Los perfiles temporales se parametrizan por operación y ancho.
Las fases posteriores conservan este perfil y lo extienden al repertorio
incorporado según su formato; la temporización no aplaza su soporte funcional.

### Fase 2.3. Recursos temporizados y arbitraje

Modelar capacidades finitas de colas, bancos, puertos y writeback, junto
con latencia e intervalo de iniciación de las unidades. Los árbitros deben
definir reservas, contrapresión y liberación de recursos, con espacio para
las respuestas que ya han sido aceptadas.

Lanes y VRF cierran esta fase con un recorrido temporizado cuyo progreso
no depende de buffers ilimitados. Cambiar un parámetro debe afectar al
recurso correspondiente y producir esperas explicables en las estadísticas.
Los contadores por recurso (accesos concedidos por banco, puerto y anchura)
permiten atribuir el tráfico del VRF en cualquier organización.

### Fase 2.4. Memoria unit-stride por bloques

La VLSU agrupa elementos respetando rango, línea, página y capacidad del
buffer. Cargas reparten datos al VRF; stores reúnen sus bytes antes del
envío. Los casos no agrupables mantienen el recorrido seguro por elemento.
La interfaz de memoria conserva identidad y datos durante los reintentos.

Se mantiene una instrucción de memoria activa, un bloque activo hasta cerrar
sus efectos en memoria y VRF, y un paquete físico en vuelo. El cierre incluye
faults, `finalVstart`, drain y contabilidad de accesos; no exige concurrencia
de memoria ni elimina su papel de barrera.

### Fase 2.5. Concurrencia desde Minor hasta las lanes

Abrir una ventana de instrucciones aritméticas independientes, con admisión
y comienzo en orden, capacidad finita y retirada arquitectónica en orden.
Frontend y scoreboard mantienen dependencias RAW/WAR/WAW conservadoras por
grupos completos; el consumidor espera a que termine todo el productor.

Minor debe permitir el solapamiento real del programa, no sólo trabajo
interno paralelo para una instrucción. El cierre exige progreso simultáneo
de instrucciones independientes y preservación de flags, faults y drain.
Las barreras de memoria y contexto siguen vigentes; no hay chaining.

### Fase 2.6. FPU y varias ALUs por lane

Incorporar FPU FP32 y sus adaptadores funcionales, redondeo y retorno de
flags. Extender las ALUs con Mul/Div/resto enteros `.vv` y `.vx` en los cuatro
anchos, definiendo ocupación, latencia y disponibilidad de cada operación.

Cada lane arbitra lecturas y writeback sin confundir independencia de las
unidades con puertos infinitos. El cierre exige recorrido arquitectónico
completo para el repertorio acordado y efectos observables al variar el
número de ALUs. No se añaden unidades enteras dedicadas ni FMA por anticipado.

### Fase 2.7. Interconexión y slides

Definir el transporte común y construir el crossbar detrás de él. Las lanes
y los ejecutores usan identidad de transferencia, aceptación/retry, entrega,
capacidad, drain y estadísticas sin conocer rutas ni arbitraje de la red.

Implementar ambas slides sin predicación, incluyendo `vstart`, tail,
adaptación del escalar y restricciones de solapamiento del manual. El cierre
demuestra que el tráfico que cruza lanes usa la red modelada y que sus
consumidores no dependen del crossbar. No se exige una segunda topología.
La red publica el tráfico por enlace con métricas comunes a cualquier
topología, de modo que otra red pueda compararse con las mismas unidades.

### Fase 2.8. Consolidación y preparación de la siguiente etapa

Cerrar la matriz por operación, forma, ancho y configuración, con evidencia
y límites explícitos. Revisar propiedad de buffers, reservas, liberación,
errores y drain en todo el recorrido. Las estadísticas deben permitir
atribuir esperas a dependencias, unidades, VRF, memoria o interconexión.
La consolidación reúne la evidencia de grupos enteros y fraccionales
incorporados en 2.2 y mantenidos por las ampliaciones posteriores.

Entregar al hito 3 un mapa de contratos que deben ampliarse: disponibilidad
por rangos, predicación, resultados escalares, operaciones colectivas y
memoria concurrente. Los descriptores compartidos deben reflejar roles y
efectos sin listas duplicadas de soporte. Lo no implementado se identifica
como tal, sin declarar soporte por haber reservado un campo o una interfaz.

El cierre conserva las restricciones de seguridad del hito 2. No exige
rehacer la comprobación cerrada del hito 1 ni adelanta chaining o máscaras.
Los criterios completos se mantienen en la
[sección 15.3 del diseño][cierre-h2].

### Ejemplos al cierre: enteros de 16 bits y FP32

Se aplican las [convenciones comunes][convenciones-ejemplos].
Estos fragmentos amplían el repertorio del hito 1; no son simulaciones
ni pruebas nuevas.

**Enteros de 16 bits y comunicación entre lanes.** `a0=N>0`;
`a1=A`, `a2=B`, `a3=S`, `a4=P` y `a5=D` apuntan a arrays
de elementos de 16 bits. El bloque calcula suma y producto módulo
`2^16` y desplaza A hacia índices menores: D[i]=A[i+1], con cero al
final. Si el bloque tiene tres elementos, A=[100,200,300] y B=[2,3,4]
producen S=[102,203,304], P=[200,600,1200] y D=[200,300,0].

```asm
vsetvli t0, a0, e16, m1, tu, mu
vle16.v v1, (a1)
vle16.v v2, (a2)
vadd.vv v3, v1, v2
vmul.vv v4, v1, v2
vslide1down.vx v5, v1, zero
vse16.v v3, (a3)
vse16.v v4, (a4)
vse16.v v5, (a5)
```

Las tres operaciones leen fuentes compatibles y escriben destinos
distintos. La slide utiliza el transporte cuando el mapper sitúa los
elementos correspondientes en lanes diferentes.

**FP32: dos cálculos independientes.** `a0=N>0`; `a1=A`,
`a2=B`, `a3=P` y `a4=S` apuntan a arrays FP32.
El bloque produce `P[i] = A[i] * B[i]` y `S[i] = A[i] + B[i]`,
con el redondeo FP correspondiente. Para un bloque de tres elementos,
A=[1,2,3] y B=[0.5,1.5,2] producen P=[0.5,3,6] y S=[1.5,3.5,5].

```asm
vsetvli t0, a0, e32, m1, tu, mu
vle32.v v1, (a1)
vle32.v v2, (a2)
vfmul.vv v3, v1, v2
vfadd.vv v4, v1, v2
vse32.v v3, (a3)
vse32.v v4, (a4)
```

Las dos operaciones aritméticas pueden solaparse si hay recursos. En el
hito 2 las cargas y stores siguen siendo barreras y los consumidores esperan
al productor completo. El ensamblador no prescribe latencias ni acredita
por sí solo que el modelo haya solapado instrucciones.

## Hito 3: Solapamiento escalar-vectorial, chaining y memoria concurrente

### Objetivo y resultado

Permitir que la CPU escalar, los accesos a memoria y el cálculo vectorial
progresen simultáneamente. Los consumidores podrán usar partes ya escritas
de un resultado sin esperar al vector completo (chaining), y habrá varias
instrucciones de memoria en vuelo. Minor podrá preparar y despachar nuevos
comandos mientras otros siguen activos, sin exigir siempre vaciar la VPU.

Incorporar selección de elementos mediante máscaras (predicación),
resultados vectoriales y escalares, y operaciones que reúnen trabajo de
varias lanes. Preservar el estado correcto ante excepciones, con recursos
de capacidad finita. Preparar interfaces para agrupar lanes y sus recursos
locales en clústeres sin rediseñar el recorrido de cada instrucción.

Se trabaja primero con las instrucciones del hito 2 y sus ampliaciones de
predicación admitidas. Los nuevos opcodes HPC/ML se incorporan en 4--5.
Preparar una familia significa dejar resueltos sus contratos de datos,
planificación, transporte, terminación y recursos; no basta nombrar una
unidad futura ni reservar un directorio.

### Fase 3.1. Operandos, resultados y contexto extensibles

Ampliar los contratos de 2.1 con fuentes tipadas, lectura del destino para
acumulaciones, escalares enteros/FP, inmediatos y formatos de máscara.
Definir resultados vectoriales, máscaras empaquetadas y retornos escalares
hacia CPU. El contexto de cada comando debe permanecer estable aunque otras
instrucciones avancen.

Minor, `VectorSupport`, frontend y VRF deben compartir roles y dependencias;
no se añaden rutas de decodificación en el backend. Preparar también los
descriptores de stride, índices y anchos de índice para memoria futura.
Los descriptores deben poder expresar varios grupos de destino, necesarios
para los accesos segmentados del 4, sin activarlos en el 3.
Separar desde aquí identidad y ubicación de los participantes, siguiendo
los contratos de preparación para clústeres que integra la fase 3.7.
El cierre entrega un esquema que representa las familias de 4--5 sin
forzarlas a dos fuentes del mismo tipo ni implementar sus opcodes aún.
Se documentarán también los puntos que requeriría un futuro comando IME:
grupos de A/B/C, fases y propiedad del acumulador. Es una preparación de
contratos; la semántica y las rutas activas necesitan su propuesta específica.

### Fase 3.2. Predicación y escrituras parciales

**Qué significa predicación.** Es decidir para cada elemento del vector
si la instrucción actúa sobre él. Se parece a un `si` dentro de un bucle:
«suma estos elementos sólo donde se cumple la condición». Una máscara
guarda esa decisión con un bit por posición: `1` la habilita y `0` la
desactiva. En las instrucciones predicadas de RVV, esa máscara se lee de
`v0`; la sintaxis `v0.t` indica que se ejecutan las posiciones habilitadas.

Por ejemplo, con `vl=4`, `vstart=0` y política de conservar los elementos
desactivados (`vma=0`), una suma predicada produce:

| Dato | Posiciones 0, 1, 2 y 3 |
| --- | --- |
| Primer vector | `[10, 20, 30, 40]` |
| Segundo vector | `[1, 2, 3, 4]` |
| Máscara en `v0` | `[1, 0, 1, 0]` |
| Destino antes de la suma | `[90, 91, 92, 93]` |
| Destino después de la suma | `[11, 91, 33, 93]` |

Sólo se suman las posiciones 0 y 2; las otras conservan su valor anterior.
Con la política agnóstica (`vma=1`) no se debe depender de ese valor. La
máscara tampoco cambia la longitud `vl`: selecciona posiciones dentro del
rango que la instrucción puede procesar. Los elementos anteriores a
`vstart` y los del tail tienen sus reglas propias.

En cargas y stores, una posición desactivada no realiza su acceso a memoria
ni genera una excepción por él. Por eso no basta calcular todo y después
ocultar resultados. La referencia es el [manual][manual-rvv], secciones
**Vector Masking** y **Prestart, Active, Inactive, Body, and Tail Element
Definitions**, edición 20240411.

**Trabajo y cierre.**

Implementar lectura de `v0`, disponibilidad de sus bits y selección de
elementos activos, con actualización parcial de palabras del VRF. Distinguir
elementos anteriores a `vstart`, activos, inactivos y tail; el soporte debe
aplicar las políticas de la operación concreta. El transporte de máscaras
no puede tratarlas como otro vector de elementos SEW.

Extender las instrucciones predicables del hito 2, incluidas ambas slides,
y garantizar que los accesos de memoria desactivados no generan efectos.
Preparar el retorno escalar con aceptación, finalización y retirada por
identidad. El cierre exige coherencia entre máscara, scoreboard, escrituras
parciales y CPU; las operaciones que producen máscaras se añaden en el 4.

Aquí se integran explícitamente `mu` y `ma`. Preservar los elementos
inactivos es válido con ambas opciones; no se añade una obligación de
sobrescribirlos al seleccionar `ma`. Las operaciones que producen máscaras
en 4--5 aplican su regla particular de tail agnostic.

### Fase 3.3. Disponibilidad por rangos y chaining aritmético

Sustituir la espera RAW de todo el productor por disponibilidad de sus
rangos. Publicar qué elementos/bytes son válidos, relacionarlos con la
identidad del productor y permitir emitir una tarea consumidora cuando
todas sus fuentes necesarias estén listas.

Para esta primera fase aritmética, el consumidor leerá el VRF tras recibir
la confirmación de escritura del rango. El bypass directo entre unidades
es una optimización posterior. WAR y WAW pueden seguir siendo conservadores
para evitar renombramiento físico. Una respuesta tardía nunca podrá marcar
listo un rango de otro comando. La publicación de resultados jóvenes ante
faults de memoria se resuelve después, en 3.5.

El cierre exige que un consumidor aritmético empiece antes de terminar todo
el productor, conserve el resultado funcional y respete puertos, créditos y
contrapresión. No se confunde chaining con varias unidades trabajando para
una sola instrucción ni con concurrencia entre instrucciones independientes.

### Fase 3.4. Varias instrucciones de memoria en vuelo

Eliminar la barrera global de memoria de forma controlada, desde Minor
hasta la VLSU. Separar tres capacidades configurables: comandos de memoria
admitidos, bloques activos y peticiones físicas pendientes. Todos mantienen
identidad, reservas para respuestas y contabilidad de liberación.

Desarrollar primero concurrencia entre cargas independientes y después la
convivencia con stores. Definir orden entre memoria escalar y vectorial,
restricciones de alias, traducciones, páginas y reintentos. Cuando no pueda
probarse independencia, se conserva el orden; no se necesita especular con
direcciones para admitir más de una instrucción.

La cola de stores debe separar datos disponibles de permiso para producir
efectos externos. El cierre exige al menos dos instrucciones de memoria del
programa simultáneamente en vuelo, varios bloques/peticiones cuando haya
capacidad y una política explícita para combinaciones load/store. Dividir
una sola instrucción en paquetes concurrentes no satisface este objetivo.

La VLSU y el backend quedan preparados para conectarse a varios puertos o
memorias de primer nivel. El reparto por direcciones asigna cada byte a un
único puerto, de modo que se conservan las reglas de orden y alias de esta
fase. La configuración de referencia puede seguir usando un solo puerto.

### Fase 3.5. Integrar memoria, chaining y faults precisos

Unir carga--cálculo--store con disponibilidad por rangos. Antes de habilitar
efectos arquitectónicos, resolver cómo se retienen resultados jóvenes cuando
una carga anterior aún puede fallar. La retirada en orden por sí sola no
protege un VRF que ya se ha sobrescrito ni un store ya enviado.

La propuesta de esta fase debe elegir mecanismos de retención/publicación
o recuperación, dimensionar sus buffers y definir el tratamiento de todos
los comandos afectados. Una opción es retener versiones jóvenes en buffers
finitos hasta que sea seguro publicarlas; si se requiere renombramiento o
ROB, se presentará esa ampliación expresamente antes de implementarla.
No se da por aprobada una solución concreta en este documento.

El contrato debe garantizar los efectos permitidos de la instrucción que
falla, el `vstart` correspondiente, la ausencia de efectos jóvenes indebidos
y el cierre de respuestas ya aceptadas. Incluir flags FP, interrupciones,
reanudación y drain; los stores sólo producen efectos cuando sea seguro.

El cierre exige solapamiento útil de memoria y cálculo, chaining desde
cargas y hacia stores, y faults precisos. Los mecanismos de 3.3 y 3.4 pueden
desarrollarse antes bajo restricciones conservadoras; su integración no se
declara terminada hasta resolver esta fase. Si los faults obligan a vaciar
siempre toda la VPU, el objetivo de solapamiento sigue pendiente.

#### Progreso escalar con comandos vectoriales pendientes

Ampliar la ventana de aritmética vectorial consecutiva del hito 2 para que
el trabajo escalar independiente pueda ejecutarse y preparar operandos de
nuevos comandos mientras la VPU sigue activa. No basta admitir instrucciones
en el pipeline: sus resultados deben poder alimentar el despacho posterior
sin esperar a la finalización de todos los comandos anteriores.

La pregunta es cuándo la frontera CPU--VPU limita la entrega de trabajo,
aunque existan datos y recursos vectoriales disponibles. Este requisito
atiende O2 y O6: modelar capacidades finitas y distinguir mediante métricas
las esperas de Minor de las dependencias y la ocupación del backend.

La propuesta de implementación debe concretar:

- En Decode/Execute y el estado vectorial de Minor, la separación entre
  despacho, ejecución escalar, finalización vectorial y retirada. La ruta
  escalar actual ejecuta funcionalmente en `commitInst()`; ampliar colas o
  añadir un cursor de despacho no resuelve por sí solo ese bloqueo.
- Qué clases pueden progresar y qué dependencias exigen esperar. Incluir
  aritmética escalar, actualización de punteros/contadores y control de
  bucle, sin enviar comandos vectoriales por caminos aún especulativos.
  Definir también las restricciones de memoria escalar y vectorial.
- La propiedad del seguimiento pendiente en Minor y `CpuVectorInterface`.
  Conservar `CommandKey`, contexto y respuesta terminal única hasta cerrar
  las obligaciones, aunque la instrucción deje la cola de retirada. La VPU
  sigue recibiendo descriptores semánticos sin objetos de instrucción CPU.
- La captura estable de operandos, configuración y redondeo por comando,
  y el retorno de resultados escalares, flags, `vstart` y faults. Precisar
  sincronización ante configuración/CSRs, barreras, interrupciones, squash
  y drain; `accepted` no equivale automáticamente a retirada segura.
- El mecanismo que impide efectos escalares o vectoriales jóvenes indebidos
  cuando una instrucción anterior aún puede fallar. Comparar retirada
  temprana de clases seguras con ejecución y retención de resultados hasta
  retirada; elegir y dimensionar la solución junto con esta fase 3.5.
  No se prescribe retirada temprana general, ROB ni renombramiento.

El cierre exige una secuencia mixta en la que progrese trabajo escalar y
se despache un nuevo comando que use sus resultados mientras otro comando
vectorial anterior siga ejecutándose. Incluir control de bucle resuelto y
casos que deban esperar por dependencia o sincronización. La evidencia se
acordará con el equipo antes de crear pruebas; se sugiere contrastar una
secuencia vectorial consecutiva con otra de trabajo vectorial equivalente
intercalada con aritmética escalar y actualización de punteros.

Registrar solapamiento escalar/vectorial, ocupación del seguimiento y tiempo
con capacidad vectorial disponible pero sin despacho por bloqueo de Minor.
Este criterio exige progreso funcional útil, no sólo más entradas en colas.
Puede desarrollarse primero con aritmética bajo restricciones conservadoras;
el cierre conjunto con memoria exige las garantías de faults de esta fase.
No se exige equivalencia de ciclos con CVA6--AraXL ni se anuncia soporte ya
implementado; la elección y los cambios de código requieren su propuesta.

### Fase 3.6. Ejecución colectiva e interconexión

Definir trabajos que reúnen resultados de varias lanes: contribuciones,
estado parcial, destino final, terminación y recursos finitos. Separar el
algoritmo de reducción del transporte y permitir políticas de combinación
dependientes de la operación, incluido el orden que exija una reducción FP.

La misma interfaz modular debe transportar máscaras y datos de diferentes
clases, con arbitraje y estadísticas comunes. Establecer las rutas de acceso
para futuras permutaciones y el retorno de resultados escalares. El cierre
deja resuelto dónde vive el estado colectivo, quién lo libera y cómo drena;
las reducciones FP se incorporan en el 4 y el resto del repertorio en el 5.

### Fase 3.7. Preparación para clusterización

**Qué se quiere preparar.** Un clúster es un grupo de lanes que podría
tener su propia interconexión local, arbitraje y recursos compartidos.
Por ejemplo, una VPU de ocho lanes podría organizarse después como dos
grupos de cuatro: los intercambios dentro de un grupo usan recursos locales
y los que cruzan al otro necesitan un transporte entre clústeres.

El programa seguiría viendo los mismos registros vectoriales y ejecutando
las mismas instrucciones. No se trata aquí de crear varias VPU independientes.
Tampoco se fija todavía cuántos grupos habrá, qué recursos compartirán o
qué topología los unirá. El objetivo del hito 3 es que esas decisiones
puedan introducirse sin rehacer las instrucciones, el chaining o la VLSU.

**Trabajo que realizar.**

1. Separar identidad lógica y ubicación. Cada lane conserva una identidad
   global inequívoca; una configuración relaciona lanes y clústeres.
   `AddressMapper` sigue resolviendo los bytes del registro a lane, banco
   y posición; un servicio de ubicación determina a qué grupo pertenece
   esa lane. Los ejecutores no deducen clústeres mediante índices privados.
2. Definir una frontera para enviar trabajo, acceder a datos y recibir
   respuestas de un grupo. Mantener detrás de ella las colas, puertos y
   árbitros locales. Sustituir accesos directos a recursos ajenos por estos
   contratos; reservar un campo `clusterId` no completa la preparación.
3. Mantener globales la identidad del comando, las dependencias
   arquitectónicas, la retirada y el estado RVV. Los fragmentos locales
   informan de progreso y terminación; ningún grupo da por finalizada una
   instrucción que todavía tiene trabajo en otro. No se exige replicar
   frontend, scoreboard ni VLSU en cada clúster.
4. Preparar el transporte para destinos locales o remotos conservando
   identidad, propiedad, aceptación/retry y reserva de recepción. Cambiar
   de una red plana a una jerárquica no debe alterar los mensajes semánticos
   de una operación. Latencias y capacidades pertenecen al modelo elegido.
5. Hacer que la disponibilidad por rangos y las confirmaciones de VRF
   identifiquen al propietario de los datos. Chaining y máscaras no deben
   asumir acceso inmediato a todas las lanes. Las operaciones colectivas
   deben poder reunir contribuciones locales y después entre grupos,
   respetando el orden semántico cuando sea obligatorio.
6. En memoria, separar generación/orden de accesos y transporte de datos
   a sus propietarios. Una carga puede tener destinos en distintos grupos;
   un store puede reunir fuentes de varios. La ubicación no cambia sus
   reglas de alias, faults, publicación segura ni contabilidad por comando.
   Cada grupo podrá disponer de sus propios puertos de memoria de primer
   nivel, con la preparación de la fase 3.4.
7. Definir cómo se reúnen errores, flags, finalizaciones y drain desde los
   grupos. Contabilizar cada recurso una vez y reservar las respuestas
   aceptadas; una ampliación con varias redes deberá justificar progreso
   y ausencia de ciclos de espera antes de activarse.
8. Exponer configuración y métricas por grupo y agregadas. Validar que
   cada lane pertenece a un único grupo y que no hay lanes sin asignar.
   Agrupar las mismas lanes no cambia por sí solo VLEN, SEW ni el contenido
   arquitectónico de los registros.

**Resultado y cierre.** La implementación del hito 3 utiliza esas fronteras
con una organización inicial de un único clúster. La configuración puede
describir agrupaciones futuras y las rutas de datos y control no dependen
de que todas las lanes compartan directamente recursos. La documentación
incluye responsabilidades, mensajes, reservas y puntos de sustitución.

Como revisión propuesta al equipo, seguir una carga, una cadena aritmética,
una operación colectiva y un store para un reparto de ocho lanes en dos
grupos de cuatro. Identificar mensajes locales y entre grupos, propietario
de cada dato, finalización global y comportamiento ante retry/fault/drain.
Esta revisión de contratos no acredita un sistema multiclúster implementado.
El cierre también debe aportar evidencia acordada de que el recorrido con
un solo grupo funciona a través de las interfaces preparadas.

**Fuera de esta fase.** Construir una red jerárquica completa, distribuir
planificadores o unidades de memoria, elegir una política de recursos por
clúster y evaluar su escalabilidad requieren propuestas concretas.
La preparación es obligatoria en el 3; el nuevo hito 4 recoge un primer
experimento acotado con clústeres, sin prometer cualquier organización.

### Fase 3.8. Consolidación de la arquitectura y entrega al hito 4

Integrar las fases anteriores y revisar que una familia nueva pueda usar
operandos, máscaras, disponibilidad, memoria, red y retirada existentes.
Cerrar los parámetros, invariantes y límites de las nuevas capacidades.
Revisar además las fronteras de clusterización definidas en 3.7, incluyendo
ubicación de datos, transporte, disponibilidad y finalización global.
La evidencia de los mecanismos se acordará con el equipo antes de crear
pruebas; describir un contrato no acredita que el mecanismo funcione.

Registrar esperas por dependencia de rangos, tiempo hasta primer resultado,
solapamiento productor/consumidor, comandos de memoria en vuelo, bloques,
peticiones, presión de buffers y espera para publicación arquitectónica.
Incluir progreso escalar con comandos pendientes y capacidad vectorial sin
utilizar por bloqueo de despacho en Minor, según la fase 3.5.
El cierre incluye funcionamiento con capacidades pequeñas y contrapresión,
sin asumir que aumentar buffers corrige un bloqueo de protocolo.

### Criterios de cierre del hito 3

- Las instrucciones del hito 2 conservan su semántica y tienen predicación
  en las variantes que la admiten; ambas slide1 incluyen esa ampliación.
- Un consumidor usa rangos listos antes de completar todo su productor.
- Hay varias instrucciones de memoria simultáneas desde el programa, con
  capacidades separadas para comandos, bloques y peticiones físicas.
- Memoria y aritmética pueden progresar juntas, incluido chaining, con
  retirada, faults, flags, interrupciones y drain definidos y comprobables.
- Minor ejecuta trabajo escalar independiente y despacha comandos que usan
  sus resultados mientras sigue ejecutándose un comando vectorial anterior,
  con control de bucle resuelto y publicación arquitectónica segura.
- Las familias de los hitos 4 y 5 tienen contratos y recursos de ejecución
  preparados: tercer operando, máscara, escalar, colectivo y direccionamiento.
- La organización inicial de un clúster usa interfaces que separan control
  global, ubicación y recursos locales. La futura agrupación de lanes no
  requiere reescribir la semántica de instrucciones ni sus protocolos.
- No queda pendiente inventar un nuevo mecanismo global de dependencias o
  publicación arquitectónica para cada nueva instrucción de los hitos 4 y 5.

### Ejemplos al cierre: progreso escalar, chaining y máscaras

Entradas: `a0=N>0`, `a1=A`, `a2=B`, `a3=C` y `a4=K`,
con datos enteros de 32 bits. En el bloque, `C[i]=A[i]+B[i]+K+1`
módulo `2^32`.

```asm
vsetvli t0, a0, e32, m1, tu, mu
vle32.v v1, (a1)
vle32.v v2, (a2)
vadd.vv v3, v1, v2
addi t2, a4, 1
vadd.vx v4, v3, t2
vse32.v v4, (a3)
```

La suma escalar debe poder preparar el operando del nuevo comando mientras
continúa trabajo vectorial anterior. Las dos cargas pueden coexistir y el
consumidor puede usar rangos ya escritos por su productor. Estas son metas
temporales del hito 3: este mismo código puede ejecutarse serializado antes,
pero eso no demuestra progreso escalar, memoria concurrente ni chaining.

**Recorrido completo por bloques.** Con las mismas entradas del ejemplo
anterior, este bucle procesa los N elementos. Los arrays son disjuntos,
y las direcciones y N avanzan con el `vl` real, no con un tamaño fijo.

```asm
.Lh3_bloque:
    vsetvli t0, a0, e32, m1, tu, mu
    vle32.v v1, (a1)
    vle32.v v2, (a2)
    vadd.vv v3, v1, v2
    addi t2, a4, 1
    vadd.vx v4, v3, t2
    vse32.v v4, (a3)
    slli t1, t0, 2
    add a1, a1, t1
    add a2, a2, t1
    add a3, a3, t1
    sub a0, a0, t0
    bnez a0, .Lh3_bloque
```

El control debe resolverse antes de enviar comandos de la siguiente
iteración. Los cambios de configuración, las dependencias reales y los
posibles faults mantienen sus puntos de sincronización: no se promete
solapamiento en cada frontera del bucle.

**Máscara suministrada como datos.** Este fragmento es independiente:
`a0=N>0`, `a1=A`, `a2=B`, `a3=C` son arrays int32 y
`a4=M` contiene N palabras uint32 con patrón `0x55555555`.
En el entorno little-endian, los bits de `v0` activan posiciones pares.

```asm
vsetvli t0, a0, e32, m1, tu, mu
vle32.v v0, (a4)
vle32.v v1, (a1)
vle32.v v2, (a2)
vle32.v v3, (a3)
vadd.vv v3, v1, v2, v0.t
vse32.v v3, (a3)
```

C recibe A+B en posiciones pares del bloque y conserva su valor anterior
en las impares. Por ejemplo, un bloque con A=[1,2,3,4], B=[10,20,30,40]
y C inicial=[9,9,9,9] da C=[11,9,33,9].

Se carga el patrón con `vle32.v`, ya presente en el
baseline; no se presupone una instrucción de comparación o carga especial
de máscaras adicional. La máscara usa un bit por elemento, no una palabra
por elemento, aunque aquí el buffer se sobredimensiona para esa carga.

## Hito 4: Precisión mixta y evaluación de clústeres

### Objetivo y alcance

Ejecutar casos de cálculo científico y aprendizaje automático que almacenan
datos y calculan con distintas precisiones, incluidas acumulaciones en un
formato de mayor precisión que las entradas. Medir error numérico y tiempos
de ejecución, y comparar una organización plana de lanes con un primer
reparto acotado de esas lanes en dos clústeres: grupos con recursos locales
e interfaces de comunicación.

Dar prioridad a FP16 y BF16, las conversiones entre precisiones y la
acumulación en un formato más amplio. Mantener FP32 y añadir el perfil FP64
para cálculo científico. Incorporar también accesos segmentados y
compactación de datos sobre los mecanismos del hito 3.

La selección cubre todas las operaciones que usen los casos acordados,
incluidos movimiento, configuración, formas escalares y auxiliares. No se
limita a añadir una multiplicación FP16 aislada. Tampoco se exige completar
todo el repertorio FP ni ejecutar un framework ML completo en este hito.

Los trabajos de [precisión mixta][mixed-precision] y [BF16][bf16-ml]
justifican dar peso a almacenar con menos bits y calcular/acumular con más
precisión. El [estudio de FP8][fp8] motiva evaluar ese formato. Son
referencias para seleccionar casos; no definen instrucciones RISC-V.

| Perfil | Objetivo funcional del hito 4 | Referencia y límite |
| --- | --- | --- |
| FP32 y FP64 | Aritmética, FMA, comparación, reducción y conversiones del perfil seleccionado. | RVV; soporte por operación y forma, sin prometer todo FP. |
| FP16 / binary16 | Movimiento, conversión con FP32, aritmética habitual y acumulación FP32. | Semántica de Zvfh/Zvfhmin; revisar dependencias escalares y formas `.vf`. |
| BF16 | Conversión con FP32 y multiplicación-acumulación con destino FP32. | Zvfbfmin/Zvfbfwma; no interpretar BF16 como FP16 ni inventar todas las operaciones BF16. |
| FP8 E4M3/E5M2 | Evaluación obligatoria de representación, error, conversión y vía de ejecución para un caso ML. | Perfil experimental separado; fijar codificación y ruta software o extensión antes de anunciar offload. |
| FP4 de la ruta CSA seleccionada | Revisar representación, escalas, empaquetado y vía de cálculo del indexador. | Dependencia del benchmark, con ruta explícita; no implica instrucciones FP4 nativas ni ejecutar MoE. |

FP16 y BF16 ocupan 16 bits pero no son intercambiables. El descriptor de
operación debe distinguir el formato; SEW=16 por sí solo no lo identifica.
Para FP8 u otro formato solicitado por un caso, la fase 4.1 decide una ruta
reproducible. Una conversión hecha por software no se contabiliza como una
instrucción vectorial nativa de ese formato ni habilita opcodes nuevos.

### Fase 4.1. Cerrar casos ML/HPC, formatos y dependencias

Concretar los micro-kernels de widening, geometría y formatos y una primera
selección de kernels densos de BLIS compatibles con el hito. Para CSA/HCA
de DeepSeek V4 Flash, identificar operandos, formatos y etapas aislables
para referencias numéricas y experimentos de precisión. La ejecución de
los bloques de atención seleccionados corresponde al 5.

Para cada caso registrar formato almacenado, formato de cálculo,
acumulador, salida, redondeo, rango de entradas, tolerancia y todas sus
instrucciones. Cerrar el subconjunto de widening/narrowing y operaciones
FP necesario; ninguna dependencia de los casos declarados ejecutables en
el 4 se aplaza al 5. Las referencias o etapas de CSA/HCA que necesiten un
repertorio más amplio se identifican como preparación para su integración
en el 5, no como bloques completos ejecutados en este hito.

Inventariar soporte de decoder, estado escalar, adaptadores numéricos y
toolchain para FP16/BF16. Seleccionar también un experimento FP8 y concretar
su representación y ejecución. Si precisa una extensión ajena al perfil
existente, su propuesta debe definirla y aprobarse antes de implementarla;
la evaluación numérica sigue siendo una entrega aunque no haya offload.
La revisión de formatos incluye FP4 cuando corresponda a la ruta de CSA
elegida. El [informe de DeepSeek][deepseek], secciones 2.3.4 y 5.2.1,
describe BF16/FP8 en KV y FP4 en su indexador. Se verificará la etapa
concreta; no se deducen sus operandos de la precisión global del checkpoint.

### Fase 4.2. Geometría y ejecución con anchos distintos

Ampliar descriptores y validación de EEW/EMUL por operando, acceso al VRF,
fragmentación y dependencias por rangos. Una instrucción puede leer
elementos de un ancho y escribir otro; no basta reutilizar SEW para todas
sus fuentes y destinos. Mantener el layout por bytes del proyecto y
documentar restricciones de grupos y solapamiento.

Incorporar las extensiones enteras y operaciones widening/narrowing que
exijan los casos elegidos, junto con conversiones FP16--FP32 y FP32--FP64
en ambos sentidos. El cierre conserva predicación, chaining, capacidad
finita y publicación arquitectónica segura con geometrías diferentes.
No requiere añadir todas las variantes widening/narrowing de RVV.

La matriz de esta fase amplía el perfil LMUL heredado del 2 para los casos
de precisión mixta. Incluye EMUL por fuente/destino y su límite de ocho
registros, partes no utilizadas del registro fraccionario y `tu/ta/mu/ma`
según la operación. Una ampliación de geometría no activa por sí sola
todas las combinaciones SEW×LMUL.

### Fase 4.3. FPU con varios formatos y conversiones

Registrar FP16, FP32 y FP64 como perfiles explícitos, y BF16 para sus
operaciones especificadas. La CPU captura el escalar en su formato correcto;
el backend no identifica tipos sólo por tamaño. Completar movimiento y
conversión, incluidas las rutas escalares necesarias para las formas `.vf`.

Como base del perfil, incluir suma, resta, multiplicación, división,
mínimo/máximo, comparación y FMA para los formatos IEEE seleccionados.
La raíz y las estimaciones de recíproco/raíz recíproca se incluyen en las
formas que necesiten los casos. Para BF16, usar las conversiones
`vfwcvtbf16.f.f.v` y `vfncvtbf16.f.f.w` y la acumulación de su extensión;
no dar por existentes suma o división BF16 genéricas en RVV base.

El cierre deja una matriz por formato, operación y forma. Flags, casos
especiales y redondeo siguen la instrucción concreta. Los auxiliares de
gem5 que ya calculan un formato no acreditan soporte desde CPU.

### Fase 4.4. Acumulación y reducciones de precisión mixta

Incorporar FMA con lectura del destino y acumulador de mayor precisión:
FP16 a FP32 y BF16 a FP32; añadir FP32 a FP64 para el perfil científico.
Usar las operaciones definidas por las extensiones correspondientes, como
`vfwmacc` y `vfwmaccbf16`, junto con FMA del mismo ancho `vfmacc`.
El adaptador debe respetar la fusión y el redondeo que indique cada una.

Integrar reducciones de suma ordenada/no ordenada y mínimo/máximo del
perfil, y las widening que se necesiten para precisión mixta. Definir
tratamiento de máscara, inicialización, resultado y combinación de
contribuciones sin asumir un árbol válido para todas las operaciones.
El cierre permite recorrer carga--cálculo--acumulación--conversión--store
con chaining y gestionar correctamente sus dependencias y flags.

### Fase 4.5. Datos intercalados y compactación

Incorporar al menos cargas/stores unit-stride de dos campos para los anchos
seleccionados, útiles para pares o datos complejos intercalados. Cerrar
grupos de destino, máscara por segmento, reanudación y faults; no aplicar
sin revisión el contrato de una carga con un solo destino.

Añadir `vcompress.vm` para el caso de filtrado/compactación acordado,
reutilizando máscaras y transporte entre lanes. Especificar rangos,
solapamiento, tail y restricciones de `vstart`. Otros números de campos y
segmentados strided/indexed entran sólo si los casos de 4.1 los requieren.

### Fase 4.6. Calidad numérica y operadores ML

Acordar con el equipo evidencia para error, redondeo, subnormales,
overflow/underflow, NaN y flags de las operaciones seleccionadas. Registrar
por separado error del formato, algoritmo y aproximaciones de biblioteca.
Para FP8, fijar variante exacta, escalado, saturación/conversión y acumulador
antes de comparar resultados o costes.

Las etapas de CSA/HCA se describen por las operaciones que requieren.
Normalización y funciones de biblioteca, como la exponencial usada en
softmax, necesitan una ruta y un criterio de error explícitos, no un opcode
RVV supuesto. Su integración va en el 5 cuando dependa de selección,
indexación, enteras, máscaras o permutaciones aún no admitidas. En FP4 se
documentan además empaquetado, escalas y coste de conversión del indexador
elegido. El cierre distingue kernel ejecutable, referencia y experimento
de formato; no los anuncia como un mismo nivel de soporte.

### Fase 4.7. Evaluación temporal y primer experimento con clústeres

Comprobar perfiles por formato: latencia, intervalo de iniciación,
ocupación, ancho de transferencia y presión sobre VRF, memoria y red.
Contrastar tendencias de cargas de cómputo y memoria con una referencia
identificada o límites analíticos. Registrar supuestos y ajustar perfiles
cuando haya evidencia; no dejar la interpretación de rendimiento para
después de medir los kernels del 5.

Concretar un primer modelo de dos clústeres de lanes detrás de las
interfaces del 3, con recursos y enlaces finitos y reglas de progreso.
Compararlo con la organización plana manteniendo constantes lanes totales,
VLEN, repertorio y recursos cuya influencia no se quiera estudiar. La
propuesta fijará qué es local y qué se comparte; no implica duplicar
automáticamente VLSU o frontend.

Dos clústeres es el punto inicial para comprobar esas fronteras, no una
constante arquitectónica ni una partición física final. La configuración
debe permitir barrer agrupaciones compatibles. L1 designa memoria de primer
nivel; el escenario debe declarar si es caché u otro tipo de memoria.
Clústeres, puertos de memoria y red se combinan desde la configuración con
las interfaces preparadas en los hitos 2 y 3; no se exige implementar una
topología adicional.

El cierre exige equivalencia funcional y métricas comparables de tráfico
local/remoto, espera y ocupación. Una configuración con dos etiquetas sobre
el mismo crossbar sin fronteras de recursos no demuestra clusterización.
No se compromete una familia completa de topologías ni equivalencia exacta
de ciclos con Ara.
El control plano/jerárquico de esta fase compara organización lógica. No
se denomina comparación planar/3D hasta incorporar una partición tecnológica
y costes calibrados o escenarios de sensibilidad explícitos.

### Fase 4.8. Consolidación y entrega al hito 5

Cerrar matrices de formato--operación--forma--ancho y kernel--dependencias,
con evidencia acordada y perfiles temporales. Incluir configuración de
clústeres, límites de precisión mixta y rutas reales de FP8/FP4. Documentar
cómo añadir una operación a una FPU existente sin duplicar protocolos globales.

El hito 5 recibe una base numérica y de transporte implementada, no sólo
una lista de formatos posibles. Sus nuevas instrucciones reutilizan
conversión, máscaras, chaining, ejecución colectiva, retiro y transporte.
Las dependencias pendientes de bibliotecas se registran con su fase del 5.

### Criterios de cierre del hito 4

- FP16, BF16, FP32 y FP64 tienen perfiles explícitos y recorridos completos
  para las operaciones acordadas, incluidas sus dependencias escalares.
- Funcionan conversiones y acumulaciones de precisión mixta con sus reglas
  de grupos, redondeo, flags, predicación y chaining.
- Los casos acordados de ML/HPC, segmentados y compactación tienen todas
  sus instrucciones necesarias dentro del perfil ejecutable.
- FP8 tiene una evaluación reproducible y una ruta de ejecución declarada;
  se distingue el experimento numérico del soporte de instrucciones nativas.
- Los formatos de las etapas CSA/HCA seleccionadas, incluido FP4 cuando
  corresponda, tienen representación y coste de adaptación declarados.
- La organización plana y la de dos clústeres conservan resultados y ofrecen
  métricas comparables, con recursos temporizados y capacidades finitas.
- La evidencia numérica y temporal identifica supuestos, límites y las
  dependencias de bibliotecas que completará el 5.

### Ejemplo al cierre: FP16 con acumulador FP32 y salida FP16

Entradas: `a0=N>0`, `a1=A16`, `a2=B16`, `a3=C32` y
`a4=D16`. A16/B16 son IEEE binary16 y C32 contiene el acumulador FP32
inicial. El bloque actualiza C32 mediante una multiplicación-acumulación
fusionada y convierte su resultado a FP16 en D16.

```asm
vsetvli t0, a0, e16, m1, tu, mu
vle16.v v1, (a1)
vle16.v v2, (a2)
vsetvli zero, t0, e32, m2, tu, mu
vle32.v v8, (a3)
vsetvli zero, t0, e16, m1, tu, mu
vfwmacc.vv v8, v1, v2
vsetvli zero, t0, e32, m2, tu, mu
vse32.v v8, (a3)
vsetvli zero, t0, e16, m1, tu, mu
vfncvt.f.f.w v4, v8
vse16.v v4, (a4)
```

`v8--v9` forman el grupo FP32; `v1`, `v2` y `v4` no se solapan
con él. Las configuraciones e16/m1 y e32/m2 tienen el mismo VLMAX y
conservan el número de elementos de este bloque. `vfwmacc` redondea la
operación fusionada en FP32; la conversión posterior redondea a FP16.

Para un bloque de tres elementos, A16=[1,2,3], B16=[0.5,1.5,2]
y C32 inicial=[10,20,30] producen C32=D16=[10.5,23,36]. Estos valores
concretos son representables exactamente en ambos formatos.

Requiere el perfil FP16 con estas operaciones de Zvfh y grupos de dos
registros habilitados. No representa BF16 ni instrucciones FP8/FP4. El
mismo código sirve para comparar organización plana y dos clústeres:
cambian los recursos y el transporte, no las instrucciones ni el resultado.

## Hito 5: Repertorio RVV y ejecución de kernels BLIS y atención

### Objetivo y criterio de selección

Completar el subconjunto de instrucciones vectoriales RISC-V (RVV) y los
auxiliares de biblioteca necesarios para ejecutar los micro-kernels
acordados, kernels de álgebra lineal densa de BLIS y los bloques de atención
CSA/HCA de DeepSeek V4 Flash. Obtener resultados numéricos y temporales
sobre los mecanismos y formatos de los hitos 2--4.
La campaña de [micro-kernels, BLIS y CSA/HCA][benchmarks] determina la
selección. [RiVEC][rivec] y [TSVC][tsvc] quedan como bibliografía de apoyo,
sin convertirse en suites acordadas ni en un ranking universal de opcodes.
El conjunto siguiente se cerrará según las dependencias reales de esas
cargas, con compilador, opciones, entradas y ROI fijados.

Se mantiene el repertorio de los hitos anteriores. Las familias siguientes
son el objetivo de ampliación; la fase 5.1 cerrará sus formas y perfiles
exactos. Añadir una familia no autoriza automáticamente todas sus variantes.

| Familia | Instrucciones del perfil que se cierra en 5.1 | Base disponible en los hitos 3 y 4 |
| --- | --- | --- |
| Enteras habituales | `vsub`, `vand`, `vor`, `vxor`, `vsll`, `vsrl`, `vsra`, `vmin[u]`, `vmax[u]`; `vadd.vi`. | Operandos tipados y adaptadores ALU. |
| Comparación y selección | `vmseq`, `vmsne`, `vmslt[u]`, `vmsle[u]`, `vmerge`; movimientos `vmv`. | Máscaras, escrituras parciales y retorno escalar. |
| Operaciones de máscara | `vmand.mm`, `vmor.mm`, `vmxor.mm`, `vmnand.mm`, `vcpop.m`, `vfirst.m`. | Máscaras empaquetadas y ejecución colectiva. |
| Reducciones enteras | `vredsum.vs`, `vredmin[u].vs`, `vredmax[u].vs`. | Contribuciones entre lanes y resultado final. |
| FP habitual | `vfsub`, `vfmin`, `vfmax`, `vfmacc`, `vfsqrt.v`; formas `.vf` pertinentes. | FPU y formatos del 4; completar las formas que falten. |
| Comparación y conversión FP | `vmfeq`, `vmfne`, `vmflt`, `vmfle`; `vfcvt.x.f.v`, `vfcvt.xu.f.v`, `vfcvt.f.x.v`, `vfcvt.f.xu.v`. | Perfiles del 4, máscaras, redondeo y flags. |
| Reducciones FP | `vfredosum.vs`, `vfredusum.vs`, `vfredmin.vs`, `vfredmax.vs`. | Perfil numérico del 4 y ejecución colectiva del 3. |
| Permutaciones | `vslideup`, `vslidedown`, `vrgather`. | Ejecución entre lanes y red modular. |
| Memoria irregular | `vlse*`, `vsse*`, `vluxei*`, `vsuxei*`, para los anchos de datos/índices aprobados. | VLSU concurrente, direccionamiento y faults. |

La notación `[u]` incluye las variantes con y sin signo, y `*` indica el
ancho codificado de memoria. La tabla no afirma que todo nombre tenga
formas `.vv`, `.vx` y `.vi`; sólo se admitirán las definidas por el manual.
El perfil entero conservará los cuatro anchos. Las filas FP incluyen soporte
que ya entrega el 4: se reutiliza y sólo se completan las formas adicionales
que exijan los kernels del 5. FP16/BF16 y FP64 no vuelven a aplazarse.

### Fase 5.1. Cerrar el perfil y los kernels de referencia

Convertir la tabla anterior en una matriz exacta por instrucción, forma,
SEW, LMUL/EMUL, predicación y configuración legal. Cerrar los casos de
LMUL y tail y las formas `vsetvli`, `vsetivli` y `vsetvl` requeridas por la
campaña, ejecutadas en CPU. Incluir también movimientos y auxiliares que
produzca el compilador antes de prometer ejecutar un kernel.

Este cierre integra las matrices y evidencias de los hitos 2--4. La campaña
conjunta no significa que los LMUL fraccionales, `m4/m8` o tail empiecen a
admitirse en el 5; su perfil inicial se concreta y comprueba en el 2.

Fijar operaciones y revisión de BLIS, y la implementación de CSA/HCA de
DeepSeek V4 Flash que se adaptará. Delimitar etapas y ROI, prefill/decode,
dimensiones, compresión/selección y formatos de las variantes acordadas.
Revisar instrucciones auxiliares, rutas de biblioteca, memoria y entorno SE.
La integración comprende los bloques de atención, no el modelo completo.
El resultado será una relación kernel--instrucciones--fase--criterio de
aceptación; no un compromiso de ejecutar suites completas sin inspección.

### Fase 5.2. Enteras, comparación, máscaras y movimientos

Añadir operaciones ALU, formas inmediatas, comparación, selección y
movimientos del perfil. Las comparaciones producen máscaras y las
selecciones leen la máscara como operando según su semántica; no se aplica
predicación genérica a una codificación que tenga otra interpretación.

Implementar operaciones lógicas sobre máscaras y sus consultas escalares
usando los contratos de 3.1, 3.2 y 3.6. El cierre exige resultados y efectos
correctos para las formas aprobadas, sin modificar el protocolo global de
admisión, dependencias o retirada para cada opcode.

### Fase 5.3. Reducciones enteras y FP

Completar las reducciones enteras y las variantes adicionales que falten
respecto al 4, con valor inicial, contribuciones activas y escritura del
resultado final según el manual. Cada operación fijará casos vacíos,
máscara, tail, solapamientos y restricciones de
`vstart`; no se hereda sin revisión el recorrido de una suma elemento a
elemento. La red y el estado colectivo se reutilizan desde 3.6.

Reutilizar las sumas FP ordenadas/no ordenadas y mínimo/máximo del perfil
del 4, ampliando sólo formas necesarias. El cierre incluye redondeo, flags
y recursos finitos, con evidencia específica para el orden de combinación.

### Fase 5.4. Integración FP y funciones de biblioteca

Completar formas FP auxiliares que revele 5.1 y ejecutar funciones de
biblioteca que requieran BLIS y las etapas CSA/HCA seleccionadas, incluida
su normalización de puntuaciones. Mantener la distinción entre instrucción
nativa, secuencia RVV y llamada escalar. No inventar opcodes de funciones
de biblioteca para evitar sus dependencias.

Reutilizar formatos, FMA, conversiones y acumuladores del 4. Acordar error
y comportamiento de las aproximaciones, y comprobar redondeo/flags de las
instrucciones utilizadas. El cierre cubre las funciones seleccionadas y
sus dependencias de enteras, máscaras, reducción o permutación; no exige
implementar todo el repertorio FP ni una biblioteca matemática completa.

### Fase 5.5. Permutaciones y memoria irregular

Añadir slides de distancia variable y gathers sobre el transporte común,
con sus reglas de índices, rangos y solapamiento. El crossbar puede seguir
siendo la red de referencia plana; las operaciones usan la interfaz común
y se mantienen válidas para la configuración con clústeres del 4.

Extender la generación de direcciones con stride e índices, manteniendo
por separado ancho del dato y ancho del índice. No se promete cualquier
combinación EEW/EMUL por compartir nombre de familia. El cierre exige que
el nuevo direccionamiento respete predicación, orden, faults y control de
capacidad ya resueltos en el hito 3. Los segmentados y la compactación del
4 se reutilizan, sin redefinir su control para el nuevo direccionamiento.

### Fase 5.6. Validación del repertorio y evaluación de kernels

Relacionar la evidencia de cada instrucción con la matriz de 5.1 y ejecutar
únicamente las comprobaciones y benchmarks que el equipo autorice. Para
los kernels seleccionados, registrar configuración, toolchain, entradas,
ROI y criterio numérico de aceptación, especialmente para coma flotante.
Separar el kernel BLIS de packing/recorrido completo y, en CSA/HCA, la
compresión/selección de la atención medida. Los índices precalculados o
datos en mayor precisión se identifican como variantes de alcance distinto.

Medir instrucciones ejecutadas y tiempo/esperas por familia para distinguir
frecuencia de uso e impacto de rendimiento. Publicar qué variantes hacen
falta para ampliar cobertura y qué límites son del modelo o del entorno.
No se declarará RVV 1.0 completa ni fidelidad temporal con Ara por superar
un conjunto de kernels. Los perfiles no calibrados se identificarán.

### Criterios de cierre del hito 5

- Existe una matriz cerrada de instrucciones, formas, anchos y restricciones,
  con evidencia por cada combinación anunciada como soportada.
- Las nuevas operaciones reutilizan predicación, chaining, memoria y
  ejecución colectiva del 3 y los perfiles del 4, conservando faults,
  flags y drain.
- Los kernels acordados ejecutan su ROI con el resultado esperado y sin
  instrucciones auxiliares fuera del perfil declarado.
- Las tres familias acordadas tienen evidencia: cobertura de micro-kernels,
  ruta vectorial de BLIS y bloques CSA/HCA con adaptación y etapas declaradas.
- Se publican configuración, método de medida, cobertura y límites; las
  frecuencias medidas se atribuyen a esos kernels y sus entradas concretas.

### Ejemplo al cierre: carga indexada, máscara y reducción

Entradas: `a0=N>0`, `a1=I` apunta a índices uint32 y `a2=X` a
datos int32. Los índices son menores que `2^30` y señalan posiciones
válidas de X. El bloque devuelve en `a3` la suma de los valores negativos
seleccionados, módulo `2^32` y extendida con signo a RV64.

```asm
vsetvli t0, a0, e32, m1, tu, mu
vle32.v v1, (a1)
vsll.vi v2, v1, 2
vluxei32.v v3, (a2), v2
vmslt.vx v0, v3, zero
vmv.v.i v4, 0
vredsum.vs v5, v3, v4, v0.t
vmv.x.s a3, v5
```

Los índices se convierten a desplazamientos en bytes; `ei32` indica el
ancho de esos desplazamientos, mientras e32 fija el ancho del dato cargado.
La carga se hace sin máscara: el filtro se aplica después a la reducción.
`v4[0]=0` es la semilla y el resultado queda en `v5[0]`.

Por ejemplo, si el bloque abarca `I=[3,0,1]` y
`X=[5,-2,7,-4]`, devuelve `a3=-6`. Estas formas ilustran el perfil que
debe concretar la fase 5.1; no acreditan una biblioteca ni atención completas.

**Paso de una actualización de columna FP32.** Fragmento independiente:
`a0=N>0`, `a1` apunta a un bloque contiguo de A[:,k], `a2` al
bloque correspondiente de C[:,j] y `a3` al escalar FP32 B[k,j].

```asm
vsetvli t0, a0, e32, m1, tu, mu
vle32.v v1, (a1)
vle32.v v8, (a2)
flw ft0, 0(a3)
vfmacc.vf v8, ft0, v1
vse32.v v8, (a2)
```

Actualiza `C[i,j] += A[i,k] * B[k,j]` con FMA. Esta operación numérica
ya pertenece al perfil del hito 4 cuando se admite esa forma; en el 5 se
integra con el resto de instrucciones y bibliotecas de las cargas elegidas.
Es un paso ilustrativo de cálculo denso, no un kernel BLIS completo ni
un bloque de atención. La integración real debe identificar packing,
bucles, auxiliares y, en atención, la ruta de normalización y exponencial.

## Dependencias y entregas entre hitos

Las fases son un orden de integración, no un permiso para habilitar
comportamientos inseguros temporalmente. 3.1 precede a máscaras y resultados
nuevos; 3.3 y 3.4 convergen en 3.5. La ejecución colectiva depende de los
contratos tipados y de la red modular. El 4 empieza con la arquitectura
del 3 cerrada; 4.2--4.4 coordinan geometría, formatos y acumulación.
El 5 amplía el repertorio y las bibliotecas sobre esa base; los casos
ejecutables comprometidos en el 4 no dependen de una implementación del 5.
La preparación para clústeres se considera desde 3.1 al diseñar identidad
y ubicación; 3.7 integra y revisa esas fronteras sobre todos los recorridos.
No se añade sólo como una reorganización documental al final del hito.
El progreso escalar amplía la ventana del 2 y se integra en 3.5 con el
seguimiento pendiente, el orden de memoria y la publicación segura. No se
habilitan efectos jóvenes sin resolver las garantías del comando anterior.

| Entrega | Debe quedar resuelto | Debe quedar identificado para después |
| --- | --- | --- |
| Cierre del 2 | Repertorio completo del hito, recursos y perfiles, identidad, propiedad, límites y observabilidad. | Contratos a ampliar para rangos, predicación, escalares, colectivos y memoria concurrente. |
| Cierre del 3 | Chaining, memoria concurrente, progreso escalar con comandos vectoriales pendientes, predicación, colectivos, publicación segura y fronteras para clústeres. | Perfiles numéricos del 4, repertorio del 5 y experimento de clústeres. |
| Cierre del 4 | Perfiles FP, precisión mixta, datos seleccionados, calidad numérica y evaluación temporal/de clústeres. | Formas adicionales, integración BLIS y CSA/HCA del 5; alcance real de los experimentos FP8/FP4. |
| Cierre del 5 | Repertorio y kernels acordados con evidencia por forma, formato y ancho. | Instrucciones no cubiertas, restricciones y experimentos posteriores. |

## Fuera del compromiso de estos hitos

Fault-only-first, repertorio completo de punto fijo y cobertura total de
RVV requieren otra priorización. También quedan fuera renombramiento/ROB
por defecto, una familia completa de redes/clústeres y equivalencia exacta
de ciclos con una implementación Ara concreta.

Los subconjuntos de widening/narrowing, FP16/BF16, segmentados y compresión
descritos sí pertenecen al 4, junto con evaluación numérica y temporal y el
primer experimento con clústeres. Otros formatos de ML se seleccionan por
los casos de 4.1, con su semántica y vía de ejecución explícitas. Estas
exclusiones no aplazan requisitos de las variantes anunciadas como soporte.

## Continuidad de investigación y líneas paralelas

IME, la evaluación de T1/T2/T3 y la exploración CMOS 2.0 forman parte del
objetivo global. No se cancelan por no estar implementadas al cerrar el 5.
Tampoco se añaden aquí como fases de código activas de los hitos 2--5.

1. Cerrar la referencia y el subconjunto RVV+IME que usen las cargas. Definir
   geometría, acumulación y datos de un comando matricial antes de activarlo.
2. Preparar ports manuales RVV/IME de los kernels seleccionados. BLIS y las
   etapas de atención deben mantener referencia, entradas y ROI comparables.
3. Concretar candidatos parametrizables: T1 con FUs compartidas y colectivos;
   T3 con motor/puente y C residente; T2 con coste de reconfiguración y modo.
4. Incorporar modelos tecnológicos de memoria, enlaces, energía y área,
   documentando fuente, incertidumbre y restricciones físicas disponibles.
5. Explorar carga, arquitectura y tecnología con controles planares justos,
   sensibilidad y evidencia suficiente para aceptar o rechazar las ventajas.

Estas líneas pueden diseñarse, perfilarse o contrastarse mientras avanza la
base. Su activación requiere propuestas específicas, no esperar sin motivo
a implementar todo RVV. Los [objetivos globales][objetivos] fijan criterios,
fuentes y límites.

Se prevén hitos 6 y siguientes, por definir según los resultados y sin
fechas cerradas. Como mínimo deberán admitir IME en el modelo y las
organizaciones que dependen de él: unidades compartidas entre RVV e IME,
motores dedicados o reconfigurables y estado fuera del VRF. La parte de la
componibilidad que no depende de IME se prepara ya en los hitos 2--4.
Cuando el trabajo de IME esté disponible se tendrá en cuenta; el resto del
plan no depende de su fecha.

La implementación de cada fase debe proponer archivos y módulos afectados,
interfaces, decisiones y alternativas, exclusiones y validación. Si para
cumplirla aparece una necesidad que amplía el alcance, se presentará antes
de modificar código. No se reabre el baseline cerrado como fase preliminar.

## Referencias

- [Diseño detallado del hito 2][diseno-h2] y [resumen del hito 2](hito2/README.md): contratos
  y repertorio que este plan conserva.
- [Manual oficial RVV][manual-rvv] y [especificación v1.0][rvv]: referencia
  normativa para definir operaciones y sus restricciones.
- [RiVEC: simulador y suite vectorial][rivec]: contexto de evaluación y
  selección de cargas; no proporciona aquí porcentajes por opcode RVV 1.0.
- [TSVC: estudio de vectorización, SC25][tsvc]: referencia complementaria
  para necesidades de los kernels; no equivale a frecuencia dinámica global.
- [Manual vectorial, edición 20260120][manual-fp]: secciones de formatos,
  conversiones, acumulación, reducciones y extensiones FP16.
- [Extensiones BF16, versión 1.0, edición 20260120][manual-bf16]: conversiones
  y acumulación BF16 con sus dependencias; no se atribuyen a RVV base.
- [Mixed Precision Training][mixed-precision], [estudio BF16][bf16-ml] y
  [FP8 Formats for Deep Learning][fp8]: selección de casos numéricos ML.
- [Plan de benchmarks][benchmarks]: campaña acordada, fichas y distribución.
- [BLIS][blis], [DeepSeek V4 Flash][flash] e [informe técnico][deepseek]:
  fuentes de las cargas; no sustituyen al manual para definir instrucciones.

[diseno-h2]: <hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md>
[cierre-h2]: <hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[manual-rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
[rvv]: https://github.com/riscvarchive/riscv-v-spec/blob/v1.0/v-spec.adoc
[rivec]: https://arxiv.org/abs/2111.01949
[tsvc]: https://riscv.epcc.ed.ac.uk/assets/files/sc25/Lin.pdf
[convenciones-ejemplos]: <Resumen de hitos - Objetivos y alcance.md#cómo-leer-los-ejemplos>
[resumen]: <Resumen de hitos - Objetivos y alcance.md>
[manual-fp]: https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html
[manual-bf16]: https://docs.riscv.org/reference/isa/v20260120/unpriv/bfloat16.html
[mixed-precision]: https://arxiv.org/abs/1710.03740
[bf16-ml]: https://arxiv.org/abs/1905.12322
[fp8]: https://arxiv.org/abs/2209.05433
[benchmarks]: <Plan de benchmarks.md>
[blis]: https://github.com/flame/blis
[flash]: https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash
[deepseek]: https://arxiv.org/html/2606.19348
[objetivos]: <Objetivos a medio y largo plazo.md>
