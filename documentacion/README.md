# Documentación de VAMPIRE y de la VPU Ara-like

La finalidad del proyecto es estudiar VPUs de vectores largos con RVV+IME,
su entrega de datos y organizaciones que puedan beneficiarse de CMOS 2.0.
El modelo gem5 y las cargas seleccionadas deben permitir comparar
candidatos con controles planares y justificar o rechazar sus ventajas.

| Documento global | Para qué leerlo |
| --- | --- |
| [Objetivos a medio y largo plazo][objetivos] | Referencia de orientación para personas e IAs: preguntas, objetivos O1--O6, plantillas T1/T2/T3, calibración y evidencia. |
| [Resumen de hitos][resumen] | Objetivos y límites de las entregas; qué queda después del 5. |
| [Plan de evolución][plan] | Fases y criterios de los hitos 2--5 y su relación con el objetivo global. |
| [Plan de benchmarks][benchmarks] | Cargas, perfiles, ports RVV/IME y criterios de comparación. |

Lee primero los objetivos y después el hito relacionado con el trabajo.
Los hitos 2--5 son la base habilitadora, no la totalidad del resultado de
investigación. Los documentos distinguen contratos, propuestas y soporte;
una sección o un diagrama no acredita una implementación.

| Hito | Alcance y estado |
| --- | --- |
| [Hito 1](hito1/README.md) | VPU funcional, integración MinorCPU, benchmark y estadísticas de los módulos actuales. |
| [Hito 2](hito2/README.md) | Plan en ocho fases: contratos, anchos, recursos, memoria por bloques, concurrencia, FPU y ALUs, interconexión y cierre. |
| [Hito 3](<Plan de evolución - Hitos 2 a 5.md#hito-3-completar-el-modelo-ara-like-adaptado>) | Predicación, chaining, memoria concurrente y preparación para clústeres. |
| [Hito 4](<Plan de evolución - Hitos 2 a 5.md#hito-4-precisión-mixta-y-ampliaciones-para-hpcml>) | Precisión mixta, FP16/BF16/FP32/FP64, ampliaciones de datos y evaluación. |
| [Hito 5](<Plan de evolución - Hitos 2 a 5.md#hito-5-repertorio-e-integración-de-kernels-hpcml>) | Completar repertorio y bibliotecas e integrar kernels HPC/ML. |

La campaña de [benchmarks acordados][benchmarks] reúne micro-kernels RVV,
kernels densos de BLIS y los bloques de atención CSA/HCA de DeepSeek V4
Flash. Sus variantes, tamaños y ROI se concretan en fichas antes de ejecutar.

Para entender el código actual, empieza por el hito 1. Para preparar las
siguientes extensiones, consulta el hito 2 y conserva los contratos del hito 1.
El hito 3 tiene como objetivos chaining y varias instrucciones de memoria
en vuelo, junto con predicación y los contratos que necesita el repertorio
HPC/ML de 4--5. También prepara la clusterización: agrupar lanes y recursos
locales dentro de una única VPU, con interfaces que separen esa ubicación
del control global. El 4 recoge un primer experimento con dos clústeres.
Renombramiento y ROB siguen sujetos a propuestas específicas.

El baseline del hito 1 ya está comprobado y cerrado por el equipo. El plan
del hito 2 empieza en los contratos de sus ampliaciones. Su FPU se dedica
sólo a coma flotante. La multiplicación, división y resto enteros se asignan
provisionalmente a la ALU, con formas vector--vector (`.vv`) y
vector--escalar entero (`.vx`) en 8/16/32/64 bits. Las latencias se
concretarán en la propuesta de implementación. El hito 2
incluye una interconexión modular, inicialmente crossbar, y `vslide1up.vx`
y `vslide1down.vx` sin máscara en los cuatro anchos. Las topologías se podrán
comparar más adelante. Su README contiene la matriz de operaciones y
anchos, ejemplos de cálculo y los pendientes posteriores. Widening y
narrowing quedan fuera del 2 y se incorporan como subconjunto en el 4.
El primer experimento de clústeres es un punto parametrizable; no fija el
número de grupos, memorias o FUs de las plantillas futuras. IME, las
plantillas elegidas y la DSE tecnológica tienen propuestas propias.

El [benchmark](../benchmarks/baseline/README.md) explica cómo ejecutar el
ejemplo simple de cuatro elementos o los 21 casos y leer los resultados.
Los resultados, logs, binarios e informes generados son locales y están
ignorados por Git.

[Interfaces propuestas](<Interfaces propuestas.md>) conserva la entrada
citada por `AGENTS.md` y remite al documento completo del hito 1.

[resumen]: <Resumen de hitos - Objetivos y alcance.md>
[plan]: <Plan de evolución - Hitos 2 a 5.md>
[benchmarks]: <Plan de benchmarks.md>
[objetivos]: <Objetivos a medio y largo plazo.md>
