# Resumen de hitos: objetivos y alcance

Este documento resume la planificación. El hito 1 está cerrado; los demás
describen objetivos de desarrollo, no soporte ya implementado. El
[plan detallado][plan] desarrolla fases, dependencias y criterios de cierre.

La finalidad está en los [objetivos a medio y largo plazo][objetivos]:
estudiar entrega de datos en RVV+IME y comparar organizaciones candidatas
y escenarios CMOS 2.0 con controles planares. Los hitos 2--5 construyen la
base; no agotan ese resultado de investigación. Se prevén hitos 6 y
siguientes, por definir según los resultados. El repertorio de
instrucciones de 2--5 parte de un análisis preliminar y se revisará con la
caracterización de micro-kernels y cargas reales.

La campaña acordada tiene tres bloques: **micro-kernels RVV**, kernels de
álgebra lineal densa de **BLIS** y los bloques de atención **CSA/HCA de
DeepSeek V4 Flash**. El [plan de benchmarks][benchmarks] concreta sus fichas
y distribución; las antiguas listas genéricas de kernels quedan sustituidas.

| Hito | Objetivo | Resultado principal |
| --- | --- | --- |
| 1. Integración funcional de MinorCPU y VPU | Ejecutar desde MinorCPU configuración, cargas, sumas y stores vectoriales de 32 bits con una instrucción activa. | Base funcional comprobada por el equipo en modo de emulación de procesos (SE). |
| 2. Concurrencia aritmética, FP32 y recursos temporizados | Solapar aritmética vectorial independiente con enteros de 8/16/32/64 bits y coma flotante de 32 bits, y medir esperas por recursos finitos. | Unidades de cálculo, bancos y colas con tiempos explícitos; memoria por bloques y red modular. |
| 3. Solapamiento escalar-vectorial, chaining y memoria concurrente | Mantener CPU, memoria y cálculo vectorial en progreso, usando resultados parciales y varias instrucciones de memoria en vuelo sin perder precisión de excepciones. | Predicación, progreso escalar con comandos pendientes, resultados por rangos y preparación para clústeres. |
| 4. Precisión mixta y evaluación de clústeres | Ejecutar casos científicos y de aprendizaje automático con distintas precisiones y comparar su error numérico y tiempos en organizaciones planas y agrupadas. | FP16/BF16/FP32/FP64, conversiones, acumulación en mayor precisión y primer experimento acotado de dos clústeres. |
| 5. Repertorio RVV y ejecución de kernels BLIS y atención | Cubrir las instrucciones vectoriales RISC-V que necesitan los micro-kernels, el álgebra lineal de BLIS y los bloques de atención seleccionados, y ejecutar esas cargas. | Resultados numéricos y temporales de los kernels acordados, con instrucciones, formatos y límites identificados. |

### Cómo leer los ejemplos

Son fragmentos RV64 en sintaxis GNU, con registros de entrada preparados
por el programa que los invoca. El entorno habilita el estado vectorial
y FP cuando corresponda, deja `vstart=0` y usa redondeo FP RNE.
Los buffers están alineados al elemento, son válidos y no se solapan,
salvo las actualizaciones del mismo array indicadas expresamente.
No se muestran inicialización de memoria, llamada ni salida del programa.

El `vsetvli` inicial devuelve en `t0` el número real de elementos del
bloque; un fragmento sin bucle procesa sólo ese bloque, no necesariamente
todo N. Los cambios de formato posteriores pueden usar `zero` como destino
para descartar el valor devuelto, como se muestra en el hito 4.
Se requiere una configuración SEW/LMUL admitida por el modelo. Se usa
`tu,mu` para preservar tail y elementos inactivos cuando la operación
lo permite; los destinos de máscara conservan sus reglas propias.
Las operaciones sin `v0.t` no están predicadas. Las operaciones FP
deben acumular los flags correspondientes aunque aquí no se lean.

El hito 1 describe el baseline cerrado; los ejemplos de 2--5 son objetivos
de ejecución futura. Ensamblar un fragmento sólo comprueba su sintaxis:
la concurrencia, chaining y progreso escalar necesitan evidencia temporal.
Tampoco se cubren aquí reinicio con `vstart` no nulo, accesos inválidos ni
todos los casos FP; sus garantías siguen siendo requisitos del hito.

**Referencia normativa:** RVV 1.0, [manual oficial, edición
20240411][manual-ejemplos]. Secciones consultadas para estos fragmentos:

| Ejemplos | Secciones del manual |
| --- | --- |
| Todos | Configuration-Setting Instructions; Vector Operands; Vector Masking; Vector Tail Agnostic and Vector Mask Agnostic; Vector Unit-Stride Instructions. |
| Hito 1 | Vector Single-Width Integer Add and Subtract. |
| Hito 2 | Vector Single-Width Integer Multiply Instructions; Vector Single-Width Floating-Point Add/Subtract Instructions; Vector Single-Width Floating-Point Multiply/Divide Instructions; Vector Slide1down Instruction. |
| Hito 3 | Vector Single-Width Integer Add and Subtract; Mask Register Layout. |
| Hito 4 | Vector Widening Floating-Point Fused Multiply-Add Instructions; Narrowing Floating-Point/Integer Type-Convert Instructions; Zvfh: Vector Extension for Half-Precision Floating-Point. |
| Hito 5 | Vector Indexed Instructions; Vector Integer Shift Instructions; Vector Integer Compare Instructions; Vector Integer Move Instructions; Vector Single-Width Integer Reduction Instructions; Integer Scalar Move Instructions; Vector Single-Width Floating-Point Fused Multiply-Add Instructions. |

### Políticas undisturbed/agnostic y LMUL

`tu/ta` se respetan desde el baseline para las instrucciones admitidas
y en cada ampliación del hito 2. `mu/ma` adquieren efecto sobre elementos
enmascarados en la fase 3.2, al incorporar predicación. Conservar valores
es válido también con agnostic; una optimización que aproveche `ta/ma`
no tiene etapa asignada y requiere su propuesta. Los destinos de máscara
mantienen su regla particular de tail agnostic.

La fase 2.1 fija la matriz SEW×LMUL. **La fase 2.2 debe cerrar suma y
memoria con `m1/m2/m4/m8` y `mf2/mf4/mf8`** en las combinaciones del perfil
legal acordado. Se integran primero los grupos enteros y después `mf2`,
seguido de `mf4/mf8`, con una instrucción activa. La matriz exacta se
concreta en la propuesta de 2.1; los factores son objetivos acordados,
no soporte ya implementado. El baseline de referencia sigue usando `m1`.

Las fases posteriores del 2 conservan ese soporte y lo extienden a las
operaciones incorporadas según su formato. El 4 amplía la geometría para
precisión mixta y el 5 cierra la campaña conjunta. Los ejemplos `m1/m2`
no limitan el perfil.

El [plan detallado](<Plan de evolución - Hitos 2 a 5.md#políticas-tailmáscara-y-perfiles-lmul-por-hito>)
fija las garantías, el reparto y la evidencia necesaria. La referencia es
el [manual RVV 1.0, edición 20240411][manual-ejemplos], secciones de
políticas tail/mask, agrupación de registros y mapeo con LMUL fraccional.


## Hito 1: Integración funcional de MinorCPU y VPU

**Objetivo:** ejecutar programas vectoriales desde MinorCPU en modo de
emulación de procesos (SE), delegando en la VPU las cargas, sumas y stores
de enteros de 32 bits, con accesos contiguos y una instrucción activa.
La configuración vectorial mediante `vsetvli` se ejecuta en la CPU.

**Estado:** baseline comprobado y cerrado por el equipo. Es el punto de
partida; no se añade una fase para repetir su comprobación.

### Ejemplo al cierre: suma de vectores y operando escalar

Entradas: `a0=N>0`, `a1=A`, `a2=B`, `a3=C` y `a4=K`.
A, B y C contienen enteros de 32 bits. Se procesa un bloque de `t0`
elementos, con resultado `C[i] = A[i] + B[i] + K` módulo `2^32`.

```asm
vsetvli t0, a0, e32, m1, tu, mu
vle32.v v1, (a1)
vle32.v v2, (a2)
vadd.vv v3, v1, v2
vadd.vx v3, v3, a4
vse32.v v3, (a3)
```

Si el bloque tiene cuatro elementos, A=[1,2,3,4], B=[10,20,30,40]
y K=5 producen C=[16,27,38,49].

Ilustra el recorrido CPU--VPU--memoria. Las instrucciones vectoriales se
ejecutan de una en una; no se exige solapamiento ni chaining.

## Hito 2: Concurrencia aritmética, FP32 y recursos temporizados

**Objetivo:** ejecutar varias instrucciones aritméticas vectoriales
independientes a la vez, con enteros de 8/16/32/64 bits y coma flotante de
32 bits (FP32). Modelar la capacidad y los tiempos de colas, bancos,
unidades de cálculo y red para explicar sus esperas y límites de servicio.

**Alcance:** enteros de 8/16/32/64 bits, suma y Mul/Div/resto en ALUs,
FPU dedicada a FP32, varias instrucciones aritméticas independientes,
memoria unit-stride por bloques e interconexión modular con ambas slide1.
Consolida contratos, capacidades, métricas y procedimiento de extensión.
Prepara la VPU para ser componible: módulos con parámetros propios, mapeo
seleccionable, contadores por recurso y tráfico medido por enlace.

**Límite de cierre:** el consumidor espera al productor completo; memoria
mantiene una instrucción, un bloque y un paquete físico activos. Precisión
mixta y nuevos formatos FP se incorporan en el 4. La fase 2.1 fija la
matriz SEW×LMUL y la 2.2 cierra suma y memoria con los grupos enteros y
fraccionales acordados, aplicando `tu/ta`. Ese soporte se conserva en las
ampliaciones posteriores del hito.

### Ejemplo al cierre: aritmética FP32 independiente

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

El [plan detallado][plan] añade ejemplos de enteros de 16 bits y slides.

## Hito 3: Solapamiento escalar-vectorial, chaining y memoria concurrente

**Objetivo:** permitir que la CPU escalar, los accesos a memoria y el cálculo
vectorial progresen simultáneamente. Los consumidores podrán usar partes
ya escritas de un resultado sin esperar al vector completo (chaining), y
Minor podrá preparar y despachar nuevos comandos mientras otros siguen
activos. Incorporar selección de elementos mediante máscaras y preservar
el estado correcto ante excepciones, con recursos de capacidad finita.

**Alcance:** predicación, es decir, seleccionar con una máscara sobre qué
elementos actúa cada instrucción; chaining para usar resultados parciales;
varias instrucciones de memoria en vuelo y solapamiento con cálculo.
Incluye progreso escalar con comandos vectoriales pendientes: ejecutar
trabajo independiente, actualizar punteros/contadores y alimentar nuevos
comandos con sus resultados mientras la VPU sigue activa, con control de
bucle resuelto y publicación arquitectónica segura. El mecanismo concreto
de retirada o retención se definirá junto con memoria y faults precisos.
Incluye resultados escalares/máscaras, ejecución colectiva, faults precisos
y preparación para agrupar lanes en clústeres dentro de una VPU, incluidos
varios puertos o memorias de primer nivel por clúster.
La fase 3.2 integra `mu/ma` con la predicación y las escrituras parciales.

**Límite de cierre:** una organización inicial de un grupo utiliza las
interfaces preparadas. El progreso escalar exige solapamiento útil y
seguimiento hasta cerrar comandos; ampliar colas por sí solo no basta.
El experimento parametrizable de clústeres pertenece
al 4 y sus tamaños iniciales no fijan la arquitectura final.

### Ejemplo al cierre: alimentar la VPU desde trabajo escalar

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

El [plan detallado][plan] desarrolla el bucle y la predicación con máscara.

## Hito 4: Precisión mixta y evaluación de clústeres

**Objetivo:** ejecutar casos de cálculo científico y aprendizaje automático
que almacenan datos y calculan con distintas precisiones, incluidas
acumulaciones en un formato de mayor precisión que las entradas. Medir
error numérico y tiempos de ejecución, y comparar una organización plana
de lanes con un primer reparto acotado de esas lanes en dos clústeres,
es decir, grupos con recursos locales e interfaces de comunicación.

**Alcance:** FP16 y BF16 junto a FP32/FP64; conversiones widening/narrowing,
FMA, acumulación en mayor precisión, comparación y reducciones del perfil.
Por ejemplo, datos FP16/BF16 con acumulador FP32. Añade segmentados de dos
campos, compactación y los auxiliares necesarios para sus casos ejecutables.
La geometría amplía las combinaciones LMUL/EMUL del perfil anterior para
estos casos, respetando políticas y límites de grupos.

Evalúa FP8 y revisa FP4 del indexador CSA con representación y ruta explícitas.
Incluye calidad numérica, perfiles temporales y comparación de una
organización plana con un primer modelo acotado de dos clústeres. El
número de clústeres, los puertos de memoria y la red se combinan desde la
configuración, sin exigir una topología adicional.
Ese control estudia organización lógica; la comparación planar/3D requiere
además particiones y costes tecnológicos explícitos.

**Límite de cierre:** micro-kernels de precisión/geometría, kernels BLIS
compatibles y referencias por etapas de CSA/HCA. La integración de los
bloques de atención corresponde al 5. Los experimentos FP8/FP4 y el soporte
de instrucciones nativas se declaran por separado; no se exige todo FP.

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

**Objetivo:** completar el subconjunto de instrucciones vectoriales RISC-V
(RVV) y los auxiliares de biblioteca necesarios para ejecutar los
micro-kernels acordados, kernels de álgebra lineal densa de BLIS y los
bloques de atención CSA/HCA de DeepSeek V4 Flash. Obtener resultados
numéricos y temporales sobre los mecanismos y formatos de los hitos 2--4.

**Alcance:** enteras habituales, máscaras, selección, movimientos,
reducciones adicionales, slides/gathers y memoria strided/indexed.
Completa los micro-kernels acordados: memoria, gathers, máscaras,
reducciones, permutaciones, widening, LMUL, `vsetvl` y tail. Integra la
selección de kernels BLIS y los bloques CSA/HCA, con los auxiliares y
funciones de biblioteca que necesiten. La campaña conjunta de LMUL y tail
reúne y amplía la evidencia de 2--4; no aplaza su soporte a este hito.

**Límite de cierre:** instrucciones, formatos y kernels concretos con
evidencia numérica y temporal; no equivale a completar RVV ni a soportar
cualquier framework ni a ejecutar el LLM completo. Los casos ejecutables
del 4 ya deben tener sus dependencias resueltas antes de empezar este hito.

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

El [plan detallado][plan] incluye además un paso de cálculo denso con FMA.

## Medio y largo plazo: continuidad del proyecto

En paralelo se caracterizan las cargas y se preparan ports RVV/IME. Quedan
propuestas específicas para IME y sus tiles, T1 con FUs compartidas, T2
SIMD/sistólica y T3 con motor dedicado y C residente. Se priorizan estudios
T1/T3 y costes de modo T2 según los perfiles y datos disponibles.

Después se incorporan modelos tecnológicos calibrados y una exploración
conjunta de carga, arquitectura y tecnología. Los tamaños son parámetros;
las ventajas 3D deben demostrarse o rechazarse, no darse por supuestas.
Se prevén hitos 6 y siguientes para incorporarlas, por definir según los
resultados y sin fechas cerradas. Como mínimo admitirán IME en el modelo y
las organizaciones que dependen de él; la componibilidad que no depende de
IME se prepara en los hitos 2--4.

[manual-ejemplos]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
[plan]: <Plan de evolución - Hitos 2 a 5.md>
[benchmarks]: <Plan de benchmarks.md>
[objetivos]: <Objetivos a medio y largo plazo.md>
