# Hito 1: VPU funcional e integración con MinorCPU

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
