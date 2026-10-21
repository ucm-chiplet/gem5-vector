# Hito 1: Integración funcional de MinorCPU y VPU

**Objetivo:** ejecutar programas vectoriales desde MinorCPU en modo de
emulación de procesos (SE), delegando en la VPU las cargas, sumas y stores
de enteros de 32 bits, con accesos contiguos y una instrucción activa.
La configuración vectorial se ejecuta en la CPU. Este baseline está
comprobado y cerrado por el equipo.

El recorrido funcional ejecuta en modo SE `vsetvli`, `vle32.v`, `vadd.vv`,
`vadd.vx` y `vse32.v`, con enteros de 32 bits, LMUL=1 y memoria unit-stride.
Minor conserva la decodificación y el estado RVV; la VPU trabaja con
descriptores propios y una sola instrucción activa.

| Documento | Qué explica |
| --- | --- |
| [Diseño e integración](<Cambios en gem5 v25 versión mejorada.md>) | Responsabilidades, integración Minor, memoria y decisiones del baseline. |
| [Interfaces](<Interfaces propuestas.md>) | Mensajes, identidades, propiedad, aceptación, finalización, faults y drain. |
| [Notas de MinorCPU](cpuSideImportantRemarks.md) | Respuestas actuales y notas históricas de la integración. |
| [Estadísticas](Estadísticas.md) | Nombres, unidades, condiciones de incremento y relaciones entre módulos. |
| [Diagrama](graphviz.svg), [fuente Graphviz](graphviz.dot) | Organización de módulos y contratos del baseline. |
| [Benchmark](../../benchmarks/baseline/README.md) | Ejemplo simple, ejecución de los 21 casos y guía de resultados locales. |

El backend tiene además helpers de 16 bits y una FPU binary16 interna.
Esto no amplía el soporte del offload de Minor: la frontera actual no
transporta contexto FP ni devuelve `fflags`.

El benchmark verifica datos, colas parciales, aritmética módulo 2^32 y
lecturas escalares inmediatamente después de stores vectoriales.
No certifica todos los recorridos de faults, drain, checkpoints ni fidelidad
temporal a Ara. No se añaden nuevas suites de pruebas en esta entrega.

Para regenerar el diagrama desde esta carpeta:

```bash
dot -Tsvg graphviz.dot -o graphviz.svg
```

La evolución con varias instrucciones activas está en el
[hito 2](../hito2/README.md).

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

Se aplican las [convenciones de lectura y referencias normativas](<../Resumen de hitos - Objetivos y alcance.md#cómo-leer-los-ejemplos>).
Este fragmento documental no añade una prueba al baseline cerrado.
