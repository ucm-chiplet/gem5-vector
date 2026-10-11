# Documentación de la VPU Ara-like

La documentación está organizada en dos hitos. Los documentos de diseño
incluyen contratos y propuestas; la existencia de una sección o un diagrama
no acredita que esa funcionalidad esté implementada.

| Hito | Alcance y estado |
| --- | --- |
| [Hito 1](hito1/README.md) | VPU funcional, integración MinorCPU, benchmark y estadísticas de los módulos actuales. |
| [Hito 2](hito2/README.md) | Diseño propuesto de concurrencia, VMFPU, anchos, recursos temporizados, interconexión y memoria por bloques. |

Para entender el código actual, empieza por el hito 1. Para preparar las
siguientes extensiones, consulta el hito 2 y conserva los contratos del hito 1.
Las ideas de renombramiento, ROB y chaining de los documentos originales
siguen siendo extensiones futuras.

El [benchmark](../benchmarks/baseline/README.md) explica cómo ejecutar el
ejemplo simple de cuatro elementos o los 21 casos y leer los resultados.
Los resultados, logs, binarios e informes generados son locales y están ignorados por Git.

[Interfaces propuestas](<Interfaces propuestas.md>) conserva la entrada
citada por `AGENTS.md` y remite al documento completo del hito 1.
