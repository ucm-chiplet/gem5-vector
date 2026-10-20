# Resumen de hitos: objetivos y alcance

Este documento resume la planificación. El hito 1 está cerrado; los demás
describen objetivos de desarrollo, no soporte ya implementado. El
[plan detallado][plan] desarrolla fases, dependencias y criterios de cierre.

La finalidad está en los [objetivos a medio y largo plazo][objetivos]:
estudiar entrega de datos en RVV+IME y comparar organizaciones candidatas
y escenarios CMOS 2.0 con controles planares. Los hitos 2--5 construyen la
base; no agotan ese resultado de investigación.

La campaña acordada tiene tres bloques: **micro-kernels RVV**, kernels de
álgebra lineal densa de **BLIS** y los bloques de atención **CSA/HCA de
DeepSeek V4 Flash**. El [plan de benchmarks][benchmarks] concreta sus fichas
y distribución; las antiguas listas genéricas de kernels quedan sustituidas.

| Hito | Objetivo | Resultado principal |
| --- | --- | --- |
| 1. Baseline funcional | Conectar MinorCPU y VPU y ejecutar el recorrido inicial. | Base comprobada por el equipo. |
| 2. Consolidación y concurrencia inicial | Ampliar la base y modelar sus recursos. | Anchos enteros, FPU FP32, memoria por bloques y red modular. |
| 3. Arquitectura Ara-like adaptada | Resolver dependencias y solapamiento y preparar clústeres. | Predicación, chaining, memoria concurrente y contratos extensibles. |
| 4. Precisión mixta y ampliaciones HPC/ML | Dar soporte numérico y de datos a casos científicos y ML. | FP16/BF16/FP32/FP64, conversiones, acumulación, datos y evaluación. |
| 5. Repertorio e integración HPC/ML | Completar instrucciones habituales y ejecutar los kernels acordados. | Cobertura de aplicaciones sobre la arquitectura y los formatos anteriores. |

## Hito 1: baseline cerrado

Integración funcional en modo SE con `vsetvli`, cargas/stores unit-stride
y suma entera de 32 bits. Es el punto de partida; no se añade una fase
para repetir su comprobación.

## Hito 2: consolidación y concurrencia inicial

**Objetivo:** disponer de una base comprensible, parametrizable y con
recursos temporizados.

**Alcance:** enteros de 8/16/32/64 bits, suma y Mul/Div/resto en ALUs,
FPU dedicada a FP32, varias instrucciones aritméticas independientes,
memoria unit-stride por bloques e interconexión modular con ambas slide1.
Consolida contratos, capacidades, métricas y procedimiento de extensión.

**Límite de cierre:** el consumidor espera al productor completo; memoria
mantiene una instrucción, un bloque y un paquete físico activos. Precisión
mixta y nuevos formatos FP se incorporan en el 4.

## Hito 3: arquitectura Ara-like adaptada

**Objetivo:** completar los mecanismos de ejecución que necesitan las
ampliaciones posteriores.

**Alcance:** predicación, es decir, seleccionar con una máscara sobre qué
elementos actúa cada instrucción; chaining para usar resultados parciales;
varias instrucciones de memoria en vuelo y solapamiento con cálculo.
Incluye resultados escalares/máscaras, ejecución colectiva, faults precisos
y preparación para agrupar lanes en clústeres dentro de una VPU.

**Límite de cierre:** una organización inicial de un grupo utiliza las
interfaces preparadas. El experimento parametrizable de clústeres pertenece
al 4 y sus tamaños iniciales no fijan la arquitectura final.

## Hito 4: precisión mixta y ampliaciones para HPC/ML

**Objetivo:** cubrir los formatos y operaciones de los casos numéricos
seleccionados, dando peso propio a aprendizaje automático.

**Alcance:** FP16 y BF16 junto a FP32/FP64; conversiones widening/narrowing,
FMA, acumulación en mayor precisión, comparación y reducciones del perfil.
Por ejemplo, datos FP16/BF16 con acumulador FP32. Añade segmentados de dos
campos, compactación y los auxiliares necesarios para sus casos ejecutables.

Evalúa FP8 y revisa FP4 del indexador CSA con representación y ruta explícitas.
Incluye calidad numérica, perfiles temporales y comparación de una
organización plana con un primer modelo acotado de dos clústeres.
Ese control estudia organización lógica; la comparación planar/3D requiere
además particiones y costes tecnológicos explícitos.

**Límite de cierre:** micro-kernels de precisión/geometría, kernels BLIS
compatibles y referencias por etapas de CSA/HCA. La integración de los
bloques de atención corresponde al 5. Los experimentos FP8/FP4 y el soporte
de instrucciones nativas se declaran por separado; no se exige todo FP.

## Hito 5: repertorio e integración de kernels HPC/ML

**Objetivo:** completar la cobertura de instrucciones y bibliotecas para
los kernels acordados, reutilizando lo construido en 2--4.

**Alcance:** enteras habituales, máscaras, selección, movimientos,
reducciones adicionales, slides/gathers y memoria strided/indexed.
Completa los micro-kernels acordados: memoria, gathers, máscaras,
reducciones, permutaciones, widening, LMUL, `vsetvl` y tail. Integra la
selección de kernels BLIS y los bloques CSA/HCA, con los auxiliares y
funciones de biblioteca que necesiten.

**Límite de cierre:** instrucciones, formatos y kernels concretos con
evidencia numérica y temporal; no equivale a completar RVV ni a soportar
cualquier framework ni a ejecutar el LLM completo. Los casos ejecutables
del 4 ya deben tener sus dependencias resueltas antes de empezar este hito.

## Medio y largo plazo: continuidad del proyecto

En paralelo se caracterizan las cargas y se preparan ports RVV/IME. Quedan
propuestas específicas para IME y sus tiles, T1 con FUs compartidas, T2
SIMD/sistólica y T3 con motor dedicado y C residente. Se priorizan estudios
T1/T3 y costes de modo T2 según los perfiles y datos disponibles.

Después se incorporan modelos tecnológicos calibrados y una exploración
conjunta de carga, arquitectura y tecnología. Los tamaños son parámetros;
las ventajas 3D deben demostrarse o rechazarse, no darse por supuestas.
Estas líneas no reciben aquí números de hito ni fechas cerradas.

[plan]: <Plan de evolución - Hitos 2 a 5.md>
[benchmarks]: <Plan de benchmarks.md>
[objetivos]: <Objetivos a medio y largo plazo.md>
