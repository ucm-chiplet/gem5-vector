# Plan de benchmarks: micro-kernels, BLIS y atención CSA/HCA

Esta es la selección de cargas indicada por el equipo. Sustituye las listas
genéricas de ejemplos de kernels del plan anterior. Concreta qué se quiere
evaluar; no implementa ni ejecuta pruebas. El baseline del hito 1 permanece
cerrado. La distribución del desarrollo está en el [plan de hitos][plan].

Esta campaña atiende los [objetivos globales][objetivos] O1 y O6: obtener
demanda real y referencias reproducibles para estudiar entrega de datos,
RVV+IME y candidatos arquitectónicos. El rendimiento de una carga aislada
no demuestra por sí solo una ventaja CMOS 2.0.

| Bloque confirmado | Alcance de la campaña |
| --- | --- |
| Micro-kernels RVV | Loads/stores unit-stride, strided e indexed; gathers, máscaras, reducciones, permutaciones, widening, LMUL, configuración `vsetvl` y tratamiento del tail. |
| Álgebra lineal densa | Kernels procedentes de [BLIS][blis], con versión, configuración y ruta vectorial identificadas. |
| LLM | Compressed Sparse Attention (CSA) y Heavily Compressed Attention (HCA) de [DeepSeek V4 Flash][flash], como bloques de atención. |

Las familias están acordadas. Los tamaños, operaciones concretas de BLIS,
implementaciones de CSA/HCA y formatos de cada experimento se fijarán en sus
fichas. Las propuestas siguientes organizan esa selección sin atribuir
soporte actual a la VPU ni comprometer toda una biblioteca o un LLM completo.

## 1. Micro-kernels RVV

Su función es aislar mecanismos del modelo antes de interpretar cargas más
grandes. Cada caso debe identificar qué instrucción ejecuta y qué recurso
estudia; una distribución de tiempos no sustituye comprobar sus resultados.

| Familia | Qué concretar en la ficha | Disponibilidad prevista |
| --- | --- | --- |
| Loads/stores unit-stride | Ancho, alineamiento, longitud, límites de bloque/línea/página y volumen de datos. | 2; concurrencia y predicación en 3. |
| Loads/stores strided | Strides legales elegidos, patrón de direcciones y presión sobre memoria. | 5. |
| Loads/stores indexed | Ancho de índice y de dato por separado, distribución, repetición de direcciones y forma ordenada/no ordenada requerida. | 5. |
| Gathers y permutaciones | Patrón local y entre lanes/clústeres; separar gather de registros de carga indexed desde memoria. | Ambas slide1 en 2; gathers y otras permutaciones en 5. |
| Máscaras | Lectura y producción de máscaras, densidad/patrón, escrituras parciales y operaciones auxiliares. | Predicación en 3; comparación FP seleccionada en 4; resto del perfil en 5. |
| Reducciones | Operador, valor inicial, máscara y orden de combinación; distinguir suma FP ordenada/no ordenada. | Infraestructura en 3; perfil FP en 4; enteras y ampliaciones en 5. |
| Widening | Anchos de fuente/destino, grupos y solapamientos, conversión o cálculo y perfil temporal. | Subconjunto seleccionado en 4. |
| LMUL | Barrer valores legales del perfil, enteros y fraccionales según SEW/ELEN; registrar grupos y configuraciones rechazadas. | Perfil inicial en 2; geometría mixta en 4; campaña completa acordada en 5. |
| Configuración de longitud | Distinguir `vsetvli`, `vsetivli` y `vsetvl`, capturar AVL/vtype y observar `vl` y dependencias en CPU. | `vsetvli` inicial; completar las formas requeridas en 5. |
| Tail handling | Longitudes que no llenan el último fragmento y políticas de tail; distinguir tail de máscara y prestart. | En cada hito para sus operaciones; cobertura conjunta en 5. |

El [manual RVV, edición 20240411][rvv], es la referencia de operaciones,
grupos, configuración, máscara y tail. LMUL y `vsetvl` no son formatos FP.
No se suman variantes reservadas ni configuraciones incompatibles como
soporte exigido por el simple hecho de barrer un parámetro.

La campaña puede comparar instrucciones independientes y cadenas
productor--consumidor, distintos límites de comandos/bloques/peticiones y
tráfico local/remoto. Se conservarán entradas y configuración al cambiar
recursos para poder atribuir diferencias a la variable estudiada.

## 2. Kernels de álgebra lineal densa de BLIS

La fuente acordada es el [repositorio flame/blis][blis]. Para preparar una
primera selección se propone empezar por GEMM real FP32/FP64 y su kernel
interno; otras operaciones y tipos se elegirán de esa misma biblioteca.
Esta prioridad es una propuesta, no una selección adicional del equipo.

La [documentación de kernels de BLIS][blis-kernels] distingue el kernel
interno de multiplicación, que recibe paneles empaquetados, de la operación
completa. Se documentarán ambas regiones si se evalúan: tiempo de cálculo
del kernel y coste del recorrido BLIS que lo invoca. No se presentará una
multiplicación escrita aparte como un benchmark de BLIS.

Antes de implementar o ejecutar, fijar:

- Commit/tag de BLIS, operaciones y tipos de datos; identificar la rutina
  concreta y el kernel RVV seleccionado en tiempo de ejecución.
- Ruta de integración y compilación para RISC-V/SE. Una compilación correcta
  no prueba que BLIS esté usando la ruta vectorial que se quiere estudiar.
- Dimensiones, strides, almacenamiento y tamaños de bloque; incluir bordes
  y longitudes no múltiplos del bloque/vector en el conjunto acordado.
- Packing, inicialización y reserva de memoria dentro o fuera de cada ROI.
- Número de hilos y afinidad; primera propuesta de evaluación con un hilo
  para aislar la VPU, sin convertirlo en una limitación de BLIS.
- Referencia numérica, tolerancia, operaciones de coma flotante contadas y
  métricas de recursos. Una rutina escalar de respaldo se identifica.
- Para un port IME, kernel de origen, cambios de algoritmo/layout, geometría
  de tiles, formatos y subconjunto de la extensión. El port se identifica
  como adaptación de BLIS y conserva una referencia RVV comparable.

El hito 4 utiliza kernels BLIS que quepan en su perfil para estudiar FMA,
precisión, geometría y recursos. El 5 completa la selección de operaciones,
formas y dependencias de biblioteca. Admitir FP16 o BF16 en la VPU no prueba
que la versión elegida de BLIS ofrezca esas rutas: deben identificarse o
proponerse como adaptaciones explícitas.

## 3. LLM: CSA y HCA de DeepSeek V4 Flash

Las fuentes son la [ficha oficial de Flash][flash] y el
[informe técnico de DeepSeek V4][deepseek], secciones 2.3 y 5.2.1.
Se fija **V4 Flash** como variante de referencia; una actualización de la
familia no cambia automáticamente este benchmark.

CSA combina compresión de KV y selección dispersa. HCA comprime más la
caché y realiza atención densa sobre la representación comprimida; no se
debe trasladar a HCA la selección dispersa de CSA. Estas diferencias siguen
el informe, no una equivalencia supuesta con atención genérica.

El objetivo es preparar y ejecutar esos bloques con entradas controladas.
No implica cargar todo el checkpoint, ejecutar MoE o reproducir el sistema
completo de inferencia. Se propondrá una adaptación para RISC-V/SE y una
referencia numérica reproducible; los kernels GPU originales no se dan por
ejecutables directamente en la VPU.

### Descomposición para planificar el trabajo

La adaptación debe identificar los pasos efectivamente presentes en la
versión elegida. Para planificar dependencias se propone registrar:

- Compresión/actualización de KV y disposición de la caché.
- Indexador, puntuaciones y selección de posiciones en CSA, con su coste
  propio si se incluye en la ROI. Usar índices precalculados mide sólo una
  parte del bloque y se declarará como tal.
- Atención sobre entradas seleccionadas o comprimidas: productos, máscaras,
  normalización de puntuaciones y acumulación de valores según la referencia.
- Proyecciones, posicionamiento y operaciones auxiliares: señalar cuáles
  se incluyen y cuáles se entregan como entradas del bloque medido.

Se documentarán como variantes de evaluación prefill y decode, fijando
cuáles se seleccionan, batch, longitud de contexto, dimensiones, tamaños
de compresión y selección. No se introducirán tamaños ni factores como
valores aprobados hasta fijar la variante y sus entradas.

### Formatos y precisión

El informe describe KV mixto BF16/FP8 y uso de FP4 en la ruta QK del
indexador de CSA. Estos requisitos se revisan sobre la implementación
concreta; la precisión de los pesos del modelo completo no determina por
sí sola la de todos los operandos de atención. [Referencia][deepseek].

El hito 4 debe registrar, por etapa, formato almacenado, formato de cálculo,
escalas, acumulador y salida. Se incorpora FP4 a esa revisión junto con FP8;
no se atribuye soporte nativo RVV a esos formatos por almacenarlos en bytes.
Para cada formato se elegirá una ruta reproducible y se medirá su coste de
conversión/desempaquetado cuando forme parte de la ROI.

Una primera adaptación en mayor precisión puede servir como referencia o
como variante explícita. No se presenta como ejecución de la ruta original
de baja precisión ni conserva automáticamente sus costes de memoria.

El hito 5 integra los bloques CSA/HCA seleccionados. La fase de selección
debe cerrar sus dependencias reales: indexación, selección, máscaras,
reducciones, permutaciones y funciones de biblioteca. Si falta una
instrucción necesaria, se incorpora a la matriz con su propuesta; una
lista de familias RVV no acredita por sí sola soporte del bloque.

La línea IME debe identificar qué etapas de CSA/HCA se transforman en
operaciones matriciales y qué trabajo sigue siendo RVV/escalar. No se
etiqueta el bloque completo como IME por acelerar una de sus multiplicaciones.
El port y sus datos se versionarán y declararán la referencia de la extensión.

## 4. Caracterización, ports RVV/IME y demanda de datos

La presentación de objetivos plantea caracterización mediante RAVE/QEMU y
ports manuales en paralelo al desarrollo gem5. Antes de reutilizar perfiles
o trazas, fijar versión, formato, captura de instrucciones/valores/rangos y
lo que realmente representa cada evento. Esta documentación no certifica
que esas herramientas o ports estén implementados en este repositorio.

Registrar instrucciones y formas, distribución de `vl`, SEW/LMUL,
reutilización de operandos y C, patrones de acceso y tamaño de los datos.
Relacionar esos perfiles con demanda de bancos, FUs, memoria y red. La
reutilización debe medirse en la ROI, no deducirse sólo del nombre GEMM.

Para comparar versiones RVV e IME, mantener función, entradas, formato y
criterio numérico o declarar la diferencia. Registrar geometría de tiles,
lambda, layout y trabajo residual RVV/escalar del port. El número de
instrucciones de cada versión puede cambiar: se compara también trabajo
útil, datos movidos y costes de adaptación/materialización.

El objetivo de captura de tráfico es distinguir datos locales a lane/motor,
dentro de un clúster y globales/entre clústeres. Separar payload útil,
tráfico de protocolo, lecturas repetidas y tráfico en cada enlace. Los
bytes por destino de un multicast y los byte-saltos son métricas distintas;
su regla de contabilidad debe figurar en la ficha.

Un cálculo analítico de tráfico, una traza funcional y una simulación
temporizada tienen distinto alcance. Sus resultados se etiquetan y no se
presentan como validación de hardware. Un contraste con otra plataforma o
hardware real necesita una correspondencia de configuración explícita.

## 5. Distribución por hitos

| Hito | Papel de los benchmarks |
| --- | --- |
| 2 | Micro-kernels del soporte de este hito: unit-stride, anchos, ALU/FP32 y slide1, con su geometría y tail. No exige repetir el baseline cerrado. |
| 3 | Casos de predicación, chaining y varias instrucciones de memoria en vuelo; infraestructura colectiva y fronteras preparadas para clústeres. |
| 4 | Micro-kernels de precisión mixta/widening y reducciones FP; primeros kernels seleccionados de BLIS; formatos y referencias por etapas CSA/HCA; comparación plana/dos clústeres. |
| 5 | Completar micro-kernels del perfil, integración BLIS e integración de CSA/HCA con las dependencias de cada ROI resueltas. |
| Líneas coordinadas | Caracterización, ports RVV/IME, plantillas candidatas y sus perfiles; calibración y DSE posteriores con comparaciones equivalentes. |

No se exige ejecutar CSA/HCA completo en el 4 si necesita indexed/gathers o
selección del 5. Sus referencias y experimentos de formatos se distinguen
de los casos que sí deben ejecutarse al cerrar el 4.
La integración RVV del 5 no acredita la implementación IME ni la evaluación
de T1/T2/T3. Los ports se preparan en paralelo y se ejecutan en cada modelo
cuando su subconjunto y sus interfaces hayan sido aprobados e implementados.

## 6. Ficha y criterios de evaluación

Cada benchmark tendrá una ficha antes de su implementación, con:

1. Fuente y revisión exacta, rutina/etapa, adaptación y licencia aplicable.
2. Entradas, semilla/dataset, dimensiones y variante; para BLIS, packing y
   bloques; para atención, contexto, compresión y selección.
3. Toolchain y opciones, binario, instrucciones/formas y configuración RVV;
   indicar rutas escalares, emuladas o nativas realmente ejecutadas.
4. Formatos, escalas, redondeo, acumulador y criterio numérico de aceptación.
5. ROI, calentamiento y repeticiones acordados; separar preparación y cálculo
   y declarar el estado de cachés/configuración de memoria.
6. Configuración de VPU y sistema: VLEN, lanes, clústeres, FUs, VRF, colas,
   interconexión, comandos/bloques/peticiones de memoria en vuelo.
7. Resultado funcional y métricas: ciclos, trabajo útil y volumen de datos,
   instrucciones dinámicas y esperas por recurso. Para CSA/HCA, añadir el
   trabajo de selección/compresión incluido y no sólo la atención final.
8. Hito/fase, dependencias pendientes y estado: referencia, adaptación,
   ejecutable o comprobado. Un caso no soportado no se registra como pasado.
9. Objetivo O1--O6, plantilla y escenario: control planar o partición 3D,
   costes tecnológicos usados y fuente de calibración, cuando correspondan.
   Separar recursos lógicos constantes y recursos aumentados bajo presupuesto.

Los tamaños y el método de ejecución en gem5 se acordarán para que cada ROI
sea manejable y representativa del mecanismo estudiado. RiVEC y TSVC siguen
siendo referencias bibliográficas para priorizar instrucciones; no forman
parte de la campaña confirmada ni sus frecuencias sustituyen medirla.

La semántica de instrucciones sigue el manual oficial. Las bibliotecas y
artículos definen las cargas; no alteran las reglas RVV. Esta revisión sólo
actualiza documentación: no crea pruebas, scripts ni adaptaciones de código.

[plan]: <Plan de evolución - Hitos 2 a 5.md>
[blis]: https://github.com/flame/blis
[blis-kernels]: https://github.com/flame/blis/blob/master/docs/KernelsHowTo.md
[flash]: https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash
[deepseek]: https://arxiv.org/html/2606.19348
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
[objetivos]: <Objetivos a medio y largo plazo.md>
