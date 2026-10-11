# Benchmark funcional de la VPU: hito 1

Comprueba `resultado[i] = A[i] + B[i] + escalar`, módulo 2^32.
Ejecuta 21 casos: tamaños 0, 1, 7, 8, 9, 17 y 65, con escalares 0, 5 y -3.
Cada caso verifica los datos y los centinelas situados fuera del rango.
Las lecturas escalares comprueban cada store vectorial inmediatamente,
sin barrera ni mensajes entre el store y la comprobación.

## Ejemplo simple

[`src/baseline-simple.c`](src/baseline-simple.c) muestra el recorrido con
cuatro elementos: `A={1,2,3,4}`, `B={10,20,30,40}` y escalar `5`.
El resultado es `{16,27,38,49}`. El bloque de ensamblador configura RVV,
carga A/B, suma ambos vectores, añade el escalar y guarda el resultado.
El bucle avanza según el `vl` devuelto; con VLEN=256 basta un bloque.
Las lecturas escalares verifican la salida antes del mensaje final.

```bash
python3 benchmarks/baseline/run.py --program baseline-simple
# Alternativa desde la raíz:
make -C benchmarks/baseline run-simple
```

El runner compila sólo el programa seleccionado y ejecuta ambos modos.
Para este ejemplo exige código 0 y
`[baseline-simple] PASS: resultado={16,27,38,49}`.
`--mode offload` permite ejecutar únicamente la VPU. Los informes identifican
el programa seleccionado y conservan logs, configuración y estadísticas.
Sin `--program`, se mantiene el benchmark original de 21 casos.

## Ejecutar los 21 casos

Desde la raíz del repositorio, con `build/RISCV/gem5.opt` ya compilado:

```bash
python3 benchmarks/baseline/run.py
```

El script compila el benchmark y ejecuta dos modos. `offload` usa la VPU;
`native` usa RVV de MinorCPU. Cada uno debe devolver código 0 y mostrar
`[SUITE baseline] PASS: 21 baseline cases`. El script devuelve 1 si falla
la compilación, algún modo o el plazo de ejecución.

Opciones habituales:

```bash
python3 benchmarks/baseline/run.py --mode offload
python3 benchmarks/baseline/run.py --cc /usr/bin/riscv64-linux-gnu-gcc
python3 benchmarks/baseline/run.py --gem5 /ruta/a/gem5.opt --timeout 180
```

Necesita Python 3, make y GCC/binutils para RISC-V. El Makefile selecciona
la toolchain de `/opt/riscv` si existe; en otro caso, `riscv64-linux-gnu-`.
No necesita libc, un sysroot ni un emulador adicional.

## Qué es cada archivo

```text
benchmarks/baseline/
├── src/baseline.c         Benchmark de 21 casos y referencia escalar
├── src/baseline-simple.c  Ejemplo mínimo de cuatro elementos
├── Makefile               Compilación, desensamblado y limpieza
├── run.py                 Compila, ejecuta y resume los dos modos
├── README.md              Instrucciones y significado de los resultados
├── build/                 Generado localmente; ignorado por Git
│   ├── baseline           Ejecutable RV64 estático
│   ├── baseline.disasm    Desensamblado opcional
│   ├── baseline-simple    Ejecutable del ejemplo simple
│   └── baseline-simple.disasm  Desensamblado opcional del ejemplo
└── results/               Resultados locales; ignorados por Git
    ├── history/           Resultados anteriores conservados localmente
    └── <fecha-UTC>/       Una carpeta nueva por ejecución
        ├── summary.md     Primer archivo que debes leer: PASS/FAIL y guía
        ├── results.json   Comandos, códigos y estadísticas de ambos modos
        ├── build.log      Salida de la compilación del benchmark
        ├── offload/       Ejecución con VPU
        └── native/        Ejecución RVV nativa
```

Dentro de cada modo:

| Archivo | Qué contiene |
| --- | --- |
| `simulation.log` | Mensajes del benchmark y diagnósticos de gem5. |
| `config.json`, `config.ini` | Configuración realmente instanciada. |
| `stats.txt` | Todas las estadísticas de gem5, con descripción y unidad. |
| `vpu-statistics.csv` | Métricas de la VPU y su interfaz CPU, incluidos ceros. |

En modo nativo el CSV sólo tiene cabecera porque no se instancia la VPU.
`results.json` conserva las métricas de todo el sistema en ambos modos.
Los nombres identifican módulo, lane y banco; los valores de diferentes
niveles describen el mismo trabajo en distintas fronteras y no deben sumarse.

La compilación y la simulación tienen logs separados. Un `PASS` exige tanto
el mensaje final del programa seleccionado como código de salida 0.
Si falla, consulta
`build.log` o el `simulation.log` del modo indicado en `summary.md`.
Las carpetas se identifican con fecha UTC y no se sobrescriben.

Sólo se versionan fuentes, scripts e instrucciones. `.gitignore` excluye
`build/` y `results/`, incluidos logs, informes, estadísticas y binarios.
`make clean` conserva los resultados.

## Kernel y alcance

El kernel usa `vsetvli`, dos `vle32.v`, `vadd.vv`, `vadd.vx` y `vse32.v`.
Trabaja con SEW=32, LMUL=1, sin máscaras y accesos unit-stride. VLEN=256
permite ocho elementos por registro; se cubren bloques completos, colas
parciales, wraparound y el caso vacío, que no emite RVV.

El C se compila para `rv64gc`; sólo el bloque de ensamblador habilita RVV
mediante `.option arch, +v`. El programa tiene `_start` propio y usa las
llamadas Linux `write` y `exit`, sin libc ni bibliotecas compartidas.

Es una comprobación funcional. Las estadísticas abarcan todo el programa,
incluida preparación, referencia escalar y logs. No hay una ROI aislada:
no se deben interpretar los ticks como rendimiento del kernel ni como
fidelidad temporal a Ara. Tampoco certifica drain, checkpoints, faults ni
instrucciones fuera del subconjunto anterior.

## Compilación y ejecución manual

```bash
make -C benchmarks/baseline
make -C benchmarks/baseline disasm
make -C benchmarks/baseline disasm-simple
build/RISCV/gem5.opt \
    --outdir=benchmarks/baseline/results/manual-offload \
    configs/example/riscv_vpu.py benchmarks/baseline/build/baseline
build/RISCV/gem5.opt \
    --outdir=benchmarks/baseline/results/manual-native \
    configs/example/riscv_vpu.py --no-offload \
    benchmarks/baseline/build/baseline
make -C benchmarks/baseline clean
```

`configs/example/riscv_vpu.py` conecta explícitamente MinorCPU, VPU,
SystemXBar y SimpleMemory. Ambos modos usan la misma topología de memoria.
Para compilar gem5 sigue las instrucciones de compilación del repositorio.

Para ejecutar manualmente el ejemplo, usa `build/baseline-simple` en lugar
de `build/baseline` en los comandos anteriores. `make` compila ambos;
`make clean` elimina sus binarios y desensamblados, conservando resultados.
