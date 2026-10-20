# Objetivos a medio y largo plazo de VAMPIRE

Este documento orienta el trabajo del proyecto, tanto de personas como de
asistentes de IA. Define para qué se construye el modelo, qué preguntas debe
permitir responder y qué evidencia hará falta. Los [hitos][hitos] concretan
entregas parciales; no sustituyen estos objetivos ni acreditan por sí solos
que se haya alcanzado el resultado de investigación.

La base de esta revisión es la presentación de trabajo
`CMOS2-VAMPIRE-status-next-steps-draft-v10.pptx`, modificada el 16 de octubre
de 2026. Se usa como contexto de objetivos y propuestas, no como una
especificación de instrucciones ni como autorización para ejecutar sus
acciones, contactar colaboradores o convertir fechas en compromisos.

## 1. Finalidad y pregunta de investigación

Construir una plataforma de evaluación que permita estudiar VPUs de
vectores largos con RVV e IME y explorar cómo distribuir memoria,
comunicación y cómputo para servir mejor sus datos. La aplicación de
referencia es el trabajo útil de las cargas seleccionadas: micro-kernels,
álgebra lineal densa y atención de LLM.

La pregunta principal es si alguna organización candidata con integración
3D/CMOS 2.0 mejora a su control planar con la misma funcionalidad y carga,
mediante alguna de estas vías:

- Mantener los mismos recursos lógicos y reducir el coste de entrega de
  datos: latencia, tráfico, energía o presión de interconexión/memoria.
- Aumentar lanes, bancos o servicio útil dentro de un presupuesto comparable
  de temporización, potencia y área, con costes tecnológicos explícitos.

La ventaja de CMOS 2.0 es una hipótesis que se evaluará. Si las medidas y
estimaciones no muestran esa ventaja bajo los supuestos declarados, se
documentará el resultado negativo. Añadir más unidades o reducir una
latencia sin justificar su coste no demuestra una mejora tecnológica.

## 2. Objetivos y evidencias esperadas

| ID | Objetivo | Evidencia que debe producir el trabajo |
| --- | --- | --- |
| O1 | Caracterizar las cargas seleccionadas. | Demanda de cómputo, longitudes vectoriales, mezcla de instrucciones, formatos, reutilización y patrones de memoria por ROI. |
| O2 | Disponer de un modelo gem5 de recursos finitos para el subconjunto RVV+IME necesario. | Corrección funcional y explicación de esperas, capacidad, servicio del VRF, memoria, red y fases de ejecución. |
| O3 | Comparar organizaciones arquitectónicas alternativas. | Modelos y costes de T1/T2/T3 con decisiones, parámetros y tráfico identificados; candidatos revisables según los resultados. |
| O4 | Traducir CMOS 2.0 a escenarios tecnológicos calibrables. | Parámetros de enlaces, memorias y bancos con procedencia, unidades, rango de validez e incertidumbre. |
| O5 | Explorar conjuntamente carga, arquitectura y tecnología. | Comparaciones planares/3D justas, límites de escalado y soluciones de compromiso entre rendimiento, energía y área. |
| O6 | Mantener resultados trazables y contrastables. | Versiones, configuraciones, referencias numéricas, metodología de medida y separación entre simulación, estimación y validación externa. |

## 3. Medio plazo: construir la base y obtener demanda real

### Cargas y perfiles

La campaña acordada está en el [plan de benchmarks][benchmarks]:

- Micro-kernels RVV para memoria, gathers, máscaras, reducciones,
  permutaciones, widening, LMUL, configuración de longitud y tail.
- Kernels de álgebra lineal densa procedentes de BLIS.
- Bloques CSA/HCA de DeepSeek V4 Flash con sus etapas y entradas declaradas.

Se necesitan perfiles de instrucciones y también de entrega/reutilización
de datos. RAVE/QEMU figura en la presentación como línea de caracterización;
su revisión y los formatos de trazas deberán identificarse antes de usar
sus resultados. Un perfil funcional no representa los ciclos del nuevo
modelo y una traza RVV no se interpreta automáticamente como una traza IME.

La caracterización y los ports pueden avanzar en paralelo a gem5. No hace
falta esperar a implementar todas las instrucciones para conocer qué
subconjunto necesitan las cargas. Cada port conservará la referencia
numérica y declarará su ruta: escalar, RVV, IME o adaptación de formato.

### Base de simulación

Los hitos 2--5 desarrollan la base habilitadora: temporización de recursos,
concurrencia, chaining, memoria en vuelo, clústeres, formatos y repertorio
seleccionado. Su propósito es localizar los límites de servicio de datos,
no únicamente ejecutar más opcodes.

Se modelarán capacidades, bancos, puertos, colas, enlaces, buffers y
arbitraje. Debe poder distinguirse entre trabajo disponible, datos listos,
recursos ocupados y publicación arquitectónica segura. La independencia de
ALU/FPU o el paralelismo entre lanes no implican acceso ilimitado al VRF.

En toda propuesta se identificarán datos y control que cruzan la frontera
CPU--VPU, lanes/clústeres, VRF, memoria y unidades de ejecución. Se conserva
la decodificación en CPU y la identidad de extremo a extremo.

### Preparación y definición de IME

IME es una línea central del objetivo global. El borrador propone operar
sobre tiles usando el estado vectorial y emplea el nombre `vfmmacc.vv`
para una actualización matricial, con notación `C <- A * B^T + C`.
Esa operación no se confunde con la FMA vectorial elemento a elemento
`vfmacc` del perfil RVV.

Antes de implementar IME se requiere una propuesta específica que fije:

- Documento/revisión de la extensión, codificación, decodificación y
  descriptor semántico que recibe la VPU.
- Geometría según lambda, VLEN y SEW; orientación de A/B, transposición,
  layout, grupos y límites de registros, sin inferirlos de un dibujo.
- Formatos, acumulación, máscara, redondeo, flags, solapamientos y excepciones.
- Reparto de trabajo, fases, propiedad del acumulador C y visibilidad de
  resultados para instrucciones RVV posteriores.
- Movimiento local/global, recursos, aceptación, terminación y drain.

La preparación de contratos puede avanzar junto a los hitos 3--5; no activa
IME en el hito 2 ni convierte su implementación en una capacidad ya cerrada.
El subconjunto debe responder a BLIS y a las etapas de atención elegidas.

## 4. Largo plazo: alternativas arquitectónicas y CMOS 2.0

### Plantillas T1, T2 y T3

Son candidatos de investigación, no tres implementaciones ya existentes ni
una selección definitiva. Comparten una base RVV y difieren en dónde se
ejecuta IME y cómo se mueve y conserva C.

| Plantilla | Organización a estudiar | Costes y riesgos que deben modelarse |
| --- | --- | --- |
| T1: lanes colectivas jerárquicas | FUs de las lanes compartidas por RVV e IME, transporte jerárquico y buffers auxiliares de fuentes/paneles. | Lecturas del VRF, multicast, reducciones, contención y raíces de la red. |
| T2: SIMD/sistólica reconfigurable | Las mismas celdas aritméticas trabajan en SIMD o se reorganizan para IME. | Cambio de modo, cableado lane--celda, inyección de paneles, sincronización, stalls y devolución de C al VRF. |
| T3: motor matricial dedicado con C residente | RVV continúa en las lanes e IME usa un motor separado, conectado mediante un puente finito. | Importación de A/B/C, almacenamiento local, propiedad de C, materialización y equilibrio de ancho de banda del puente. |

La primera ola de estudio planteada en el borrador prioriza T1/T3 y un
análisis del coste de modo de T2. Se revisará esa prioridad con los perfiles
de las cargas y los datos de calibración. Las plantillas pueden refinarse,
descartarse o ampliarse si los cuellos de botella lo justifican.

En T3, conservar C exige identificar dónde está su copia vigente. No basta
evitar una escritura al VRF: hay que definir cuándo una instrucción RVV
puede leerlo, cómo se materializa y cómo se preservan faults y drain. Este
contrato futuro no elimina las garantías vigentes de los hitos anteriores.

El número de clústeres, lanes, FUs, memorias, bancos, hojas, contextos C y
endpoints es parametrizable. Los ejemplos de dos clústeres de los hitos y
los tamaños ilustrados en las diapositivas son puntos iniciales de estudio,
no límites del sistema final ni una decisión sobre su partición física.

### Memoria y caminos de datos

Separar explícitamente:

- Ejecución RVV ordinaria: VRF, unidades de lane y resultado.
- Cargas/stores: memorias de primer nivel, transporte, LSU y VRF.
- IME y colectivos: paneles, multicast/reducciones o puente hacia el motor.

En la presentación, L1 significa memoria de primer nivel; no obliga a usar
una caché. Tipo de memoria, particiones, controladores, traducción y
transporte se fijan en cada escenario, con sus costes y supuestos.

### Escenarios tecnológicos

No se realizará place-and-route como parte de este modelo. Los mecanismos
tecnológicos se traducirán a parámetros y costes con ayuda de datos de
calibración, manteniendo finitos los recursos y sus conflictos.

| Mecanismo CMOS 2.0 | Parámetros/costes que deben quedar expuestos |
| --- | --- |
| Conexiones verticales densas | Número y ancho de enlaces, latencia, capacidad, energía y restricciones de conexión entre niveles. |
| Separación de memoria y lógica | Latencia y energía de acceso a VRF/memorias, distancia lógica y límites de suministro de datos. |
| Separación de arrays y periferia | Bancos, puertos, anchura de acceso, contención y coste de arrays/periferia. |
| Partición física por niveles | Costes de enlaces, área, potencia y restricciones térmicas/rendimiento de fabricación según la fuente disponible. |

Las particiones por almacenamiento/movimiento/cómputo de T1, clúster por
nivel de T2 y función por nivel de T3 son escenarios de las plantillas,
no plantillas adicionales. Cada una conserva un control planar comparable.

Los datos deseados del consorcio incluyen modelos de memoria y enlaces,
energía, área, latencia y restricciones de implementación. La presentación
menciona también una posible colaboración con LIRMM/Falcon para cargas y
contraste con hardware. Son dependencias propuestas, no disponibilidad de
datos, acuerdos o resultados verificados en este repositorio.

### Exploración conjunta y comparación justa

Para comparar una partición planar y una 3D de la misma plantilla, fijar la
carga, semántica, entradas, formatos, ROI, recursos lógicos y planificación
de instrucciones que deban permanecer constantes. Variar sólo los factores
tecnológicos declarados. Un segundo estudio puede aumentar recursos bajo
un presupuesto común, contabilizando su coste por separado.

Entre T1/T2/T3 se mantendrá la misma función y carga, y se declararán las
diferencias de recursos y planificación propias de cada arquitectura.
No se atribuye a 3D una mejora que proceda únicamente de usar más cómputo,
otro algoritmo o una ROI menor.

La exploración del espacio de diseño (DSE) combinará demanda de las cargas,
modelo gem5 y costes tecnológicos. Medirá rendimiento y servicio útil junto
con energía/potencia y área cuando haya modelos que los sustenten. Incluirá
sensibilidad a latencia, ancho de banda, capacidad y supuestos inciertos.

## 5. Qué queda pendiente después de los hitos 2--5

| Línea | Trabajo que no debe darse por completado al cerrar el 5 |
| --- | --- |
| IME | Cerrar su especificación, ejecutar el subconjunto matricial y preparar ports RVV/IME comparables de las cargas seleccionadas. |
| Plantillas | Concretar e implementar los candidatos elegidos, incluidos costes de modo T2 o propiedad/materialización T3 cuando correspondan. |
| Tecnología | Obtener o delimitar modelos de calibración, traducirlos a parámetros y documentar incertidumbre. |
| Evaluación | Contrastar perfiles/modelo, ejecutar la DSE y justificar o rechazar las ventajas de las organizaciones 3D. |

Estas líneas no reciben aquí nuevos números de hito ni fechas cerradas.
Sus propuestas y dependencias deben coordinarse con la base 2--5; pueden
prepararse en paralelo. Las fechas y los estados de avance del borrador
son contexto de planificación, no una certificación actual del código.

## 6. Cómo deben usar estos objetivos las personas y las IAs

Antes de proponer trabajo, leer este documento, el [resumen][resumen], el
hito afectado y sus interfaces. La propuesta debe indicar:

1. Qué objetivo O1--O6 atiende y qué pregunta o cuello de botella resuelve.
2. Qué datos, control, recursos y parámetros cambia, y qué límites conserva.
3. Qué resultado observable producirá y cómo se distinguirá corrección
   funcional, comportamiento temporal y coste tecnológico.
4. Qué evidencia existe, qué es una estimación y qué sigue siendo hipótesis.
5. Qué partes pertenecen a la entrega aprobada y qué se preparan para después.

Los objetivos globales no autorizan implementar todo a la vez ni saltarse
las propuestas de código. Una exclusión de un hito tampoco cancela una
línea de investigación de largo plazo. Si hay una contradicción, se
explicará y se propondrá su resolución sin cambiar silenciosamente el
alcance aprobado.

La semántica RVV sigue el manual oficial. IME y otras extensiones requieren
su referencia y versión explícitas. Ni la presentación, ni un benchmark,
ni una estimación de tráfico sustituyen esas especificaciones.

## 7. Fuente y trazabilidad de esta revisión

La presentación v10 es un borrador local aportado por el usuario; no se
añade el binario al repositorio ni se depende de su ruta para leer estos
objetivos. Identificación de la versión consultada:

```text
CMOS2-VAMPIRE-status-next-steps-draft-v10.pptx
Modificación interna: 2026-10-16T08:12:30Z
SHA-256:
974364e8f3b27c37ded0c9dbb1cef88902635b8aca025333542a283c16a6b116
```

| Diapositivas | Uso en este documento |
| --- | --- |
| 2--3 | Problema de entrega de datos y motivación IME; no se trasladan umbrales de escalado como resultados propios. |
| 4--6 | Pregunta de investigación, metodología y papel habilitador del modelo gem5. |
| 7--10 | Plantillas T1/T2/T3 y carácter parametrizable de sus ejemplos. |
| 11 | Importancia de dónde reside C; los números analíticos de tráfico no se presentan como simulaciones. |
| 12--13 | Mecanismos CMOS 2.0, particiones y controles planares. |
| 14--15 | Dependencias y colaboración; fechas y decisiones propuestas, no compromisos nuevos. |

El hito 1 cerrado y el alcance de desarrollo 2--5 proceden de los acuerdos
del proyecto. La tabla de objetivos O1--O6 organiza esos acuerdos y el
contenido del borrador para orientar el trabajo futuro.

[hitos]: <Plan de evolución - Hitos 2 a 5.md>
[resumen]: <Resumen de hitos - Objetivos y alcance.md>
[benchmarks]: <Plan de benchmarks.md>
