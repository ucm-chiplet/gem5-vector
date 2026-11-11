# Pathfinder para la VPU (cierre del hito 2)

Adaptación del Pathfinder de [RiVEC][rivec] (RISC-V Vectorized Benchmark
Suite), que a su vez parte de Rodinia. Calcula, fila a fila, el camino de
coste mínimo en una matriz:
`dst[n] = wall[t+1][n] + min(dst[n-1], dst[n], dst[n+1])`.

Es la carga de referencia del cierre del hito 2 (criterio 16 del
[diseño][cierre]). Su kernel usa exactamente 7 tipos RVV: `vsetvli`,
`vle32.v`, `vse32.v`, `vslide1up.vx`, `vslide1down.vx`, `vmin.vv` y
`vadd.vv`, con SEW=32 y LMUL=m1.

`make check` compila el binario y comprueba su perfil RVV; `run.py` lo
ejecuta en gem5 con la VPU y con el RVV nativo de MinorCPU y contrasta
ambos resultados (paquetes P2 y P3 del [plan de cierre de la fase 8][plan]).

## Procedencia

| Dato | Valor |
| --- | --- |
| Repositorio | <https://github.com/RALC88/riscv-vectorized-benchmark-suite> |
| Revisión | `32a14ddfb9acaeb677800e8d2e18a45d9421d4cc` (4 de noviembre de 2024) |
| Último cambio en `_pathfinder` | `bb30dca` (17 de octubre de 2024) |
| Fuente adaptada | `_pathfinder/src/pathfinder.cpp` |
| Entradas copiadas | `_pathfinder/input/data_tiny.in` (32×32) y `data_small.in` (128×1024) |
| Licencias | [LICENSE-RODINIA](LICENSE-RODINIA) (Universidad de Virginia) y [LICENSE-RIVEC](LICENSE-RIVEC) (Barcelona Supercomputing Center) |

Las entradas y las licencias se copian sin cambios, salvo el salto de
línea final que exigen los hooks del repositorio. `data_medium.in` (1 MB)
y `data_large.in` (4 MB) no se copian; pueden descargarse de la misma
revisión y colocarse en `input/` para usar `INPUT=medium` o
`INPUT=large`.

## Cambios respecto al original

1. **C sin libc** en lugar de C++ con glibc. La glibc del sysroot de
   `/opt/riscv` está compilada con V: al compilar el original aparecen
   unos 45 tipos RVV en `fscanf`, `memcmp`, `strftime` y otras funciones,
   que con offload serían instrucciones no soportadas. El arranque
   (`_start`) y la salida (llamadas Linux `write`/`exit`) siguen el
   [baseline del hito 1](../baseline/README.md).
2. **Entrada incrustada.** `tools/embed_input.py` convierte el `data_*.in`
   en una cabecera C al compilar; el programa no lee ficheros.
3. **Verificación completa.** Se comparan todos los elementos con la
   referencia del fichero y con la versión escalar (`run()` del original),
   calculada en el mismo programa. El `compare()` original devuelve tras el
   primer elemento, de modo que sólo comprobaba uno. Se ha contrastado
   fuera del simulador que la referencia de `tiny` y `small` coincide con el
   algoritmo escalar.
4. **`NUM_RUNS` configurable**, 1 por defecto (el original usa 100). Cada
   repetición recalcula el mismo resultado.
5. **Región de medida con m5ops** en lugar de `gettimeofday`:
   `m5_reset_stats` y `m5_dump_stats` rodean sólo `runVector()`. Se
   emiten como instrucciones (`.long`, opcode `0x7b`) para no enlazar
   `libm5`; `-DPATHFINDER_ROI=0` las elimina. Además se imprime una
   huella del resultado (`checksum`) para comparar modos.
6. **`runVector` es `noinline`**, para que el kernel conserve su símbolo y
   `check_profile.py` pueda exigir que todo el RVV esté en él.

El cuerpo del kernel conserva los intrinsics, su orden y la actualización
en sitio de `dst`. Los elementos vecinos entre tiras (`aux`, `aux2`) se
leen con loads escalares justo después de stores vectoriales, lo que
ejercita el orden de memoria entre CPU y VPU.

## Compilar y comprobar el perfil

```bash
make -C benchmarks/pathfinder check                 # INPUT=tiny, NUM_RUNS=1
make -C benchmarks/pathfinder INPUT=small check
make -C benchmarks/pathfinder NUM_RUNS=4 check
```

Cada combinación se construye en `build/<entrada>-runs<N>/`, ignorado por
Git: la cabecera generada, el binario `pathfinder` y su desensamblado
`pathfinder.disasm`. `check` falla si aparece un tipo RVV distinto de los
7, si falta alguno o si hay RVV fuera de `runVector`.

Opciones de compilación:

```text
clang --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d -std=c11 -O2
      -fno-vectorize -fno-slp-vectorize -Wall -Wextra -Werror
      -ffreestanding -fno-builtin -fno-stack-protector
      -nostdlib -static --ld-path=<ld de RISC-V> -Wl,-e,_start
```

`-O2 -fno-vectorize -fno-slp-vectorize` son las opciones acordadas en el
[plan de benchmarks][benchmarks]. Se enlaza con el `ld` de la toolchain
GNU de `/opt/riscv` (o `riscv64-linux-gnu-ld`), porque no siempre hay `lld`
instalado; `LINKER`, `CLANG` y `OBJDUMP` permiten cambiar las herramientas.

Toolchain usada al preparar el paquete: Clang 20.1.8 (Fedora 20.1.8-4.fc42),
`llvm-objdump` 20.1.8 y GNU ld 2.46 de `/opt/riscv`.

## Ejecutar en gem5

Con `build/RISCV/gem5.opt` compilado, desde la raíz del repositorio:

```bash
python3 benchmarks/pathfinder/run.py                  # tiny, ambos modos
python3 benchmarks/pathfinder/run.py --input small
python3 benchmarks/pathfinder/run.py --mode offload \
    --config-arg=--vpu-alus-per-lane=2
```

`run.py` y `sweep.py` usan el runner común de
[`benchmarks/common/vpu_bench.py`](../common/vpu_bench.py); el perfil RVV
lo comprueba [`benchmarks/common/check_profile.py`](../common/check_profile.py).

El runner ejecuta `make check` y no simula si el perfil RVV falla. Después
ejecuta `offload` (VPU) y `native` (`--no-offload`). Un modo pasa si el
programa devuelve 0 y escribe `PASS`; en offload, además, la VPU debe
completar todos los comandos que despacha Minor y la red debe entregar
todo lo aceptado. Con ambos modos, las huellas deben coincidir.
`--config-arg` pasa opciones de la VPU a `riscv_vpu.py` sólo en offload.

Los resultados quedan en `results/<fecha>/`, ignorado por Git:
`summary.md` (resultado y métricas del kernel por modo), `results.json`,
`build.log` y, por modo, `simulation.log`, `stats.txt` (primer volcado:
kernel; último: programa completo), `roi-statistics.csv` y la
configuración instanciada.

## Barrido de configuraciones

`sweep.py` ejecuta el modo offload con varias configuraciones de la VPU y
compara tiempo, métricas clave y huella con la primera (`base`):

```bash
python3 benchmarks/pathfinder/sweep.py                 # tiny, lista por defecto
python3 benchmarks/pathfinder/sweep.py --input small --only lanes4
python3 benchmarks/pathfinder/sweep.py --configs mis-configs.json
```

La lista por defecto varía un eje cada vez (ALUs, bancos, puertos,
lanes, red, bloque de memoria, ventana de Minor y comandos activos) con
`--vpu-set RUTA=VALOR` y `--minor-vector-window` de `riscv_vpu.py`. El
resultado queda en `results/sweep-<fecha>/summary.md`. El análisis de
estos datos está en
[Observabilidad - análisis con Pathfinder](<../../documentacion/hito2/Observabilidad - análisis con Pathfinder.md>).

## Salida esperada

El programa devuelve 0 y escribe, por ejemplo:

```text
[pathfinder] checksum: sum=1731 xor=29
[pathfinder] PASS: 32x32, runs=1
```

Si falla, devuelve 1 e indica la primera discrepancia
(`[pathfinder] FAIL: vector[i]=..., reference=...`).

[rivec]: https://github.com/RALC88/riscv-vectorized-benchmark-suite
[cierre]: <../../documentacion/hito2/Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[plan]: <../../documentacion/hito2/Plan de cierre de la fase 8.md>
[benchmarks]: <../../documentacion/Plan de benchmarks.md>
