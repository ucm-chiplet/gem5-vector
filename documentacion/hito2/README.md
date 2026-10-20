# Hito 2: arquitectura temporizada y concurrencia

Este hito es un diseño propuesto, organizado en ocho fases de trabajo.
El baseline del hito 1 ya está comprobado y cerrado; no se incluye una fase
para repetirlo. Los cambios aquí descritos son documentales y no implementan
los módulos ni la microarquitectura del hito 2.

- [Diseño del hito](<Hito 2 - Concurrencia, FPU, anchos e interconexión.md>):
  objetivos, responsabilidades, contratos y decisiones de arquitectura.
- [Fases de trabajo](<Hito 2 - Concurrencia, FPU, anchos e interconexión.md#13-fases-de-trabajo-y-dependencias>):
  orden, tareas, interfaces, resultados y criterios de cierre por fase.
- [Plan de evolución: hitos 2 a 5](<Plan de evolución - Hitos 2 a 5.md>):
  objetivos, fases, dependencias y criterios de cierre de las cuatro etapas.
- [Resumen de hitos](<Resumen de hitos - Objetivos y alcance.md>):
  lectura breve de sus objetivos, alcance y límites.
- [Plan de benchmarks](<Plan de benchmarks.md>): micro-kernels RVV,
  kernels densos BLIS y atención CSA/HCA de DeepSeek V4 Flash.
- [Diagrama objetivo](graphviz_hito2.svg) y
  [fuente Graphviz](graphviz_hito2.dot): organización propuesta.
- [Baseline del hito 1](../hito1/README.md): punto de partida funcional.

El recorrido previsto es:

1. Definir contratos, formatos, estados y capacidades.
2. Generalizar suma y memoria a 8/16/32/64 bits.
3. Temporizar recursos finitos y arbitraje del VRF.
4. Agrupar memoria unit-stride en bloques con buffers y un bloque activo.
5. Solapar instrucciones independientes desde Minor hasta las lanes.
6. Incorporar la FPU FP32, operaciones enteras Mul/Div/resto en la ALU y
   varias ALUs por lane.
7. Añadir transporte modular entre lanes, inicialmente con un crossbar, y
   `vslide1up.vx` y `vslide1down.vx` como consumidores obligatorios.
8. Consolidar soporte, contratos, observabilidad y procedimiento de
   extensión; documentar la entrega técnica al hito 3.

La FPU sólo ejecuta coma flotante: suma, multiplicación y división FP32.
La multiplicación, división y resto enteros se asignan provisionalmente a
la ALU, sin unidades dedicadas adicionales. El objetivo entero es admitir
8/16/32/64 bits, tanto vector--vector (`.vv`) como vector--escalar entero
(`.vx`). Los perfiles temporales se concretarán en la propuesta de fase 6.
Una división no tiene por qué ocupar el mismo tiempo que una suma. Cada fase
requerirá una propuesta y aprobación de código independientes.

## Instrucciones y anchos previstos al terminar el hito 2

Esta matriz describe el resultado que se quiere construir, no el soporte
actual del código. **Objetivo** significa que esa combinación forma parte
del hito. **—** significa que queda fuera. Para anunciar soporte habrá que
completar el recorrido CPU--VPU--VRF/memoria--CPU de la forma concreta.

**Predicación** significa decidir en qué posiciones del vector actúa una
instrucción mediante una máscara: cada bit habilita o desactiva un elemento,
como un `si` por posición. Sin predicación participan todos los elementos
del rango ejecutable. El hito 3 incorporará esa selección; el plan incluye
una [explicación con ejemplo][predicacion].

| Operación | Forma prevista | 8 bits | 16 bits | 32 bits | 64 bits |
| --- | --- | --- | --- | --- | --- |
| Suma entera `vadd` | `.vv`, `.vx` | Objetivo | Objetivo | Objetivo | Objetivo |
| Multiplicación entera `vmul` | `.vv`, `.vx` | Objetivo | Objetivo | Objetivo | Objetivo |
| División entera `vdiv`, `vdivu` | `.vv`, `.vx` | Objetivo | Objetivo | Objetivo | Objetivo |
| Resto entero `vrem`, `vremu` | `.vv`, `.vx` | Objetivo | Objetivo | Objetivo | Objetivo |
| Cargas y stores unit-stride | `vle8/16/32/64.v`, `vse8/16/32/64.v` | Objetivo | Objetivo | Objetivo | Objetivo |
| Suma FP `vfadd` | `.vv` | — | — | Objetivo FP32 | — |
| Multiplicación FP `vfmul` | `.vv` | — | — | Objetivo FP32 | — |
| División FP `vfdiv` | `.vv` | — | — | Objetivo FP32 | — |
| Desplazamiento de un elemento `vslide1up`, `vslide1down` | `.vx`, sin predicación | Objetivo | Objetivo | Objetivo | Objetivo |

`vsetvli` permanece en CPU y configura `vl` y `vtype` para las combinaciones
legales y soportadas. Las columnas describen el ancho del elemento, no VLEN
ni el ancho físico de una ALU. En memoria se mantiene EEW=SEW y no se habilitan
otros modos de direccionamiento. Las formas `.vv` usan dos vectores; `.vx`
usa un vector y el escalar capturado por CPU.

En concreto, multiplicación incluye `vmul.vv` y `vmul.vx`; división incluye
`vdiv.vv`, `vdiv.vx`, `vdivu.vv` y `vdivu.vx`; resto incluye `vrem.vv`,
`vrem.vx`, `vremu.vv` y `vremu.vx`. Todas estas formas tienen como objetivo
los cuatro anchos enteros de la matriz.

`vslide1up.vx` y `vslide1down.vx` son objetivos de fase 7 en los cuatro anchos,
inicialmente sin predicación (`vm=1`). La suma binary16 existente es una
capacidad interna y no añade offload FP16 a la matriz.

## Cómo operan los distintos anchos

En la aritmética de este hito, **fuentes vectoriales y resultado tienen el
mismo ancho**. La CPU normaliza el escalar de `.vx` según RVV. Cambiar SEW
cambia cuántos bits usa cada elemento, sin convertir la operación en widening.

| Operación | Resultado por elemento |
| --- | --- |
| `vadd` | Los SEW bits bajos de la suma; aritmética módulo `2^SEW`. |
| `vmul` | Los SEW bits bajos del producto; no guarda el producto completo ni su mitad alta. |
| `vdiv` / `vdivu` | Cociente con signo / sin signo, del mismo ancho. |
| `vrem` / `vremu` | Resto con signo / sin signo, del mismo ancho. |
| Carga / store | Mueve los bytes del elemento sin convertir su valor. |
| Operaciones FP32 | Resultado FP32 con redondeo y flags; se aplican en CPU al retirar. |

Por ejemplo, con elementos de 8 bits, `200 + 100` conserva `44` y `20 * 20`
conserva `144`. Un producto de elementos de 16 bits devuelve 16 bits; obtener
un resultado de 32 bits sería widening y queda aplazado. Admitir enteros de
64 bits tampoco habilita FP64.

La división con signo trunca hacia cero: `-7 / 3 = -2` y su resto es `-1`.
La división por cero devuelve un cociente con todos los bits a uno y el
resto conserva el dividendo. En el overflow con signo (mínimo representable
dividido por `-1`), el cociente conserva ese mínimo y el resto es cero. Estos
casos siguen RVV y deben resolverse sin comportamiento indefinido del host.
Referencias: [multiplicación y división vectoriales RVV][rvv] y
[reglas de división entera RISC-V][riscv-div].

Todos los objetivos están sujetos a la legalidad de grupos y configuración.
La geometría debe alojar los elementos habilitados; 64 bits requiere una
palabra local de al menos ocho bytes. Capacidad de buffers, ocupación,
latencia e intervalo de iniciación se comprueban por operación y ancho;
admitir los mismos tamaños no significa tener los mismos tiempos.

## Slides: alcance al cerrar el hito y pendientes de RVV

Las dos slides conservan el ancho y mueven patrones de bits; no usan la FPU.
Con `vl=4`, `vstart=0`, fuente `[10, 20, 30, 40]` y escalar `99`:

| Instrucción | Resultado | Acción |
| --- | --- | --- |
| `vslide1up.vx` | `[99, 10, 20, 30]` | Desplaza a índices mayores e inserta el escalar en 0. |
| `vslide1down.vx` | `[20, 30, 40, 99]` | Desplaza a índices menores e inserta el escalar en `vl-1`. |

La referencia normativa es el
[manual oficial RVV 1.0, edición 20240411][manual-rvv], secciones
**Vector Slide Instructions**, **Vector Slide1up** y
**Vector Slide1down Instruction**. Esta tabla describe el alcance que se
exigirá al terminar el hito; no enumera código que falta hoy:

| Aspecto de la especificación | Cobertura al cerrar el hito 2 |
| --- | --- |
| Formas y anchos | Ambas `.vx` en 8/16/32/64 bits, sin predicación. |
| Escalar desde XLEN | Extender con signo si XLEN < SEW; conservar bits bajos si XLEN > SEW. |
| `vl=0` o `vstart>=vl` | No actualizar ningún elemento del destino. |
| `vstart` no nulo | Preservar índices anteriores y ejecutar sólo el rango activo; ejecución aislada según el plan de Minor. |
| Inserción del escalar | En up, sólo si el índice 0 está activo; en down, si `vl-1` está activo. |
| Tail | Respetar `vta`; preservar el tail es válido incluso cuando se permite sobrescribirlo. |
| Solapamiento up | Validar que los grupos fuente/destino no se solapan; el manual reserva la codificación si se solapan. |
| Solapamiento down | Admitir solapamientos legales, incluido el mismo registro, preservando fuentes aún necesarias. |
| Grupos | Validar alineamiento, capacidad y LMUL legales dentro del perfil configurado. |
| Máscaras (`vm=0`) | Pendiente para otro hito: requiere aplicar la máscara del destino y las políticas `vma` correspondientes. |

Por tanto, para ampliar estas dos instrucciones a sus variantes con
predicación quedará pendiente leer `v0`, decidir qué destinos se escriben y
aplicar la política de elementos enmascarados sin modificar los que deban
preservarse. No se podrá anunciar cobertura completa de ambas instrucciones
incluyendo máscaras al cerrar este hito.

`vstart`, tail, adaptación del escalar y los solapamientos legales no se
aplazan: son requisitos de las variantes sin máscara admitidas. Las
combinaciones fuera del perfil deben identificarse antes de producir
efectos. Extender el perfil de geometría/LMUL será una ampliación explícita,
sin atribuirle aquí cobertura de cualquier configuración RVV.

Las slides no obligan a que cada elemento cruce lanes. `AddressMapper`
determina cuáles necesitan la red y cuáles permanecen locales; el transporte
no debe depender de la topología concreta. Otra topología o una latencia
distinta cambia el modelo temporal, no la semántica RVV.

Las slides de distancia variable, las formas FP `.vf`, gathers y SLDU
completa son operaciones adicionales pendientes; no son requisitos que
falten para ejecutar estas dos variantes enteras sin predicación.

## Interconexión modular entre lanes

Las lanes y los ejecutores usarán una interfaz común de transporte:
admisión, `Accepted`/`Retry`, entrega por identidad, capacidad, drain y
estadísticas. La topología implementa conectividad, encaminamiento,
arbitraje, buffers y temporización detrás de esa interfaz. El crossbar es
el primer modelo, no una dependencia fija de los consumidores.

La implementación se seleccionará al configurar `VectorEngine`. Cambiarla
más adelante no deberá exigir reescribir las instrucciones, las lanes o los
mensajes semánticos. Los receptores correlacionarán transferencias por clave
y no supondrán orden de llegada. En este hito se define la separación y se
usa el crossbar; comparar anillo, malla u otras topologías será trabajo
posterior, con su propuesta y validación.

## Pendientes para después del hito 2

El [plan de evolución](<Plan de evolución - Hitos 2 a 5.md>) distribuye
los pendientes sin reducir el alcance actual de este hito:

- **Hito 3, completar la arquitectura:** operandos y resultados tipados,
  predicación, escrituras parciales, disponibilidad por rangos y chaining;
  varias instrucciones de memoria en vuelo, con capacidades separadas para
  comandos, bloques y peticiones; solapamiento con cálculo y faults precisos;
  contratos y recursos de ejecución colectiva para las familias de 4--5.
  Incluye las variantes con máscara de ambas slide1. Chaining y memoria
  concurrente son objetivos de cierre, no optimizaciones opcionales.
  También prepara la [clusterización][clusters]: agrupar lanes y recursos
  locales dentro de una VPU, separando ubicación, transporte y control
  global. El 3 dejará las interfaces preparadas con un único grupo inicial;
  el primer experimento multiclúster se concreta en el nuevo hito 4.
- **Hito 4, precisión mixta y ampliaciones HPC/ML:** FP16/BF16 junto a
  FP32/FP64, conversiones entre anchos, FMA y acumulación en mayor precisión,
  reducciones y auxiliares del perfil; segmentados y compactación. Incluye
  evaluación FP8 y revisión de FP4 para CSA, calidad numérica y perfiles
  temporales; comparación de la organización plana con un modelo acotado
  de dos clústeres. Se admiten
  las operaciones de los casos seleccionados, sin exigir todo FP.
- **Hito 5, repertorio e integración HPC/ML:** enteras habituales,
  comparación, selección y máscaras; reducciones adicionales, slides de
  distancia variable, gathers y memoria strided/indexed. Completa formas FP
  y bibliotecas necesarias para los kernels acordados, reutilizando el 4.
  Completa la campaña de micro-kernels, BLIS y bloques CSA/HCA.
- **Ampliaciones posteriores sin compromiso en estos hitos:**
  fault-only-first, repertorios completos RVV o de punto fijo y una familia
  completa de topologías/clústeres. Renombramiento, ROB y equivalencia de
  ciclos con una referencia concreta requieren propuestas específicas.

Al cerrar el 2 se documentarán los roles de operandos, tipos de resultado,
efectos y puntos de ampliación que recibirá el 3. Preparar un contrato no
equivale a habilitar instrucciones, máscaras o chaining antes de su fase.

Los perfiles temporales de Mul/Div/resto y de las dos slides se concretan
durante el propio hito 2. Son decisiones pendientes de sus fases, no
ampliaciones que se trasladan automáticamente al hito siguiente.

## Benchmarks acordados

La campaña se organiza en tres bloques:

- **Micro-kernels:** loads/stores unit-stride, strided e indexed; gathers,
  máscaras, reducciones, permutaciones, widening, LMUL, `vsetvl` y tail.
- **Álgebra lineal densa:** kernels de la biblioteca BLIS.
- **LLM:** CSA y HCA de DeepSeek V4 Flash como bloques de atención.

El [plan de benchmarks](<Plan de benchmarks.md>) es la referencia común de
selección, fuentes, fichas y reparto por hitos. El 2 evalúa su repertorio;
el 3 aporta mecanismos de solapamiento, el 4 los perfiles numéricos y
primeras cargas compatibles, y el 5 completa la integración de la campaña.

## Lectura del diagrama y métricas

En el diagrama, la vista superior muestra el recorrido global. El panel
inferior amplía una de las mismas lanes, con ALUs y FPU independientes y
writeback compartido; no añade otra lane. Las marcas M y T remiten a los
servicios comunes de mapeo y soporte. La memoria conserva un único bloque
activo hasta cerrar sus efectos tanto en memoria como en el VRF.

Las estadísticas ya disponibles se describen en
[hito 1: estadísticas](<../hito1/Estadísticas.md>). Las métricas futuras de
conflictos de puertos, bloques o tráfico entre lanes sólo tendrán significado
cuando se implementen sus recursos correspondientes.

## Regenerar el diagrama

Para regenerar el diagrama desde esta carpeta:

```bash
dot -Tsvg graphviz_hito2.dot -o graphviz_hito2.svg
```

[rvv]: https://github.com/riscvarchive/riscv-v-spec/blob/v1.0/v-spec.adoc
[riscv-div]: https://docs.riscv.org/reference/isa/unpriv/m-st-ext.html
[manual-rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
[predicacion]: <Plan de evolución - Hitos 2 a 5.md#fase-32-predicación-y-escrituras-parciales>
[clusters]: <Plan de evolución - Hitos 2 a 5.md#fase-37-preparación-para-clusterización>
