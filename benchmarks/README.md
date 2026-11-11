# Benchmarks de la VPU

[Baseline del hito 1](baseline/README.md): suma vectorial de enteros de
32 bits, con un ejemplo simple de cuatro elementos y un benchmark de
21 casos. Ambos verifican una referencia escalar y se ejecutan en MinorCPU
con VPU o con RVV nativo.

```bash
python3 benchmarks/baseline/run.py --program baseline-simple
python3 benchmarks/baseline/run.py
```

[Pathfinder](pathfinder/README.md): carga del cierre del hito 2, adaptada
de RiVEC sin libc. Compila con Clang y comprueba que el binario sólo
contiene los 7 tipos RVV del kernel.

```bash
make -C benchmarks/pathfinder check
```

[Concurrencia aritmética](concurrency/README.md): micro-kernel con cuatro
operaciones independientes por tira, para observar comandos ejecutándose
a la vez en la VPU y el efecto de ALUs, puertos y buffers.

```bash
python3 benchmarks/concurrency/run.py
python3 benchmarks/concurrency/sweep.py --variant mixed
```

[`common/`](common/): runner y barrido compartidos (`vpu_bench.py`),
comprobación del perfil RVV (`check_profile.py`) y arranque sin libc con
m5ops (`vpu_runtime.h`).

Las fuentes y los scripts se suben a Git. Los binarios y los resultados
permanecen en carpetas locales ignoradas: `*/build/` y `*/results/`. Cada ejecución genera su propia guía `summary.md`.
