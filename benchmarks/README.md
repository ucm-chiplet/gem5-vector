# Benchmarks de la VPU

[Baseline del hito 1](baseline/README.md): suma vectorial de enteros de
32 bits, 21 casos con referencia escalar y ejecución en MinorCPU con VPU
o con RVV nativo.

```bash
python3 benchmarks/baseline/run.py
```

Las fuentes y los scripts se suben a Git. Los binarios y los resultados
permanecen en carpetas locales ignoradas: `baseline/build/` y
`baseline/results/`. Cada ejecución genera su propia guía `summary.md`.
