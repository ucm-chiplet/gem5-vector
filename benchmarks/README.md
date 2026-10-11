# Benchmarks de la VPU

[Baseline del hito 1](baseline/README.md): suma vectorial de enteros de
32 bits, con un ejemplo simple de cuatro elementos y un benchmark de
21 casos. Ambos verifican una referencia escalar y se ejecutan en MinorCPU
con VPU o con RVV nativo.

```bash
python3 benchmarks/baseline/run.py --program baseline-simple
python3 benchmarks/baseline/run.py
```

Las fuentes y los scripts se suben a Git. Los binarios y los resultados
permanecen en carpetas locales ignoradas: `baseline/build/` y
`baseline/results/`. Cada ejecución genera su propia guía `summary.md`.
