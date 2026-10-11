# Hito 2: arquitectura temporizada y concurrencia

Este hito es un diseño propuesto. En esta entrega se organiza su
documentación y se conservan los contratos; sus módulos nuevos y su
microarquitectura no se implementan.

- [Diseño y entregas](<Hito 2 - Concurrencia, VMFPU, anchos e interconexión.md>):
  concurrencia, dependencias, anchos, bancos, VMFPU, memoria por bloques y red.
- [Diagrama objetivo](graphviz_hito2.svg) y
  [fuente Graphviz](graphviz_hito2.dot): organización propuesta.
- [Baseline del hito 1](../hito1/README.md): punto de partida funcional.

Las estadísticas ya disponibles se describen en
[hito 1: estadísticas](<../hito1/Estadísticas.md>). Las métricas futuras de
conflictos de puertos, bloques o tráfico entre lanes sólo tendrán significado
cuando se implementen sus recursos correspondientes.

Para regenerar el diagrama desde esta carpeta:

```bash
dot -Tsvg graphviz_hito2.dot -o graphviz_hito2.svg
```
