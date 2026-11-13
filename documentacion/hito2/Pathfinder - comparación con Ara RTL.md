# Hito 2: Pathfinder, comparación de la VPU con Ara RTL

Comparación aprobada por el usuario el 12 de noviembre de 2026: ejecutar
**el mismo kernel** de [Pathfinder][benchmark] en Ara RTL (Verilator) y en
la VPU de gem5, comprobar que ambos hacen el mismo trabajo y explicar la
diferencia de ciclos.

Atiende O1 (caracterizar la carga), O2 (explicar esperas, memoria y VRF
del modelo) y O6 (resultado trazable frente a una referencia externa).
**No es una calibración:** gem5 no se ajusta a Ara y los parámetros por
defecto no cambian. Es la primera referencia concreta para la
comprobación de perfiles frente a Ara que el
[plan de evolución][plan] sitúa en el hito 4.

## Decisiones

| Decisión | Valor |
| --- | --- |
| Mismo kernel | `pathfinder.c` se divide en kernel, datos, comprobaciones y arranque; un único `kernel.o` se enlaza en los dos programas |
| Compilador del kernel | Clang 20.1.8, opciones acordadas en el [plan de benchmarks][bench] más `-mcmodel=medany -mno-relax` |
| Escenarios de gem5 | `base`, `memAra` (latencia de memoria igualada), `memAra+banks8` (8 bancos por lane) y `memAra+banks8+win8` (8 instrucciones en vuelo, como Ara); `native` como referencia |
| Línea temporal por instrucción | Nuevo flag de depuración `VectorTimeline` en la VPU |
| Ubicación | Herramientas en `benchmarks/pathfinder/` (`ara/`, `compare_ara.py`, `tools/compare_kernel.py`); este documento en el hito 2 |

## Condiciones

| Dato | Valor |
| --- | --- |
| gem5 | `ac60b56930` con los cambios de esta entrega; `riscv_vpu.py`: MinorCPU a 1 GHz sin cachés, `SimpleMemory`, VPU de 2 lanes, VLEN 256, 2 bancos por lane |
| Ara | `34bd3bc1` (pulp-platform/ara) con la corrección local de compiler-rt del `Makefile` raíz; CVA6 con cachés L1; 2 lanes, VLEN 256 (`build_2l_v256`) |
| Verilator | 5.047 (instalado por `make verilator` de Ara) |
| Compiladores | Objetos comunes: Clang 20.1.8 (Fedora). Runtime de Ara y `main_ara.c`: LLVM 20.1.0 de Ara |
| Entradas | `tiny` (32×32) y `small` (128×1024), `NUM_RUNS=1`, datos alineados a 64 bytes (`ALIGN=64`) |
| Región medida | `runVector()`. gem5: `m5_reset_stats`/`m5_dump_stats`. Ara: CSR `cycle` tras `fence`, antes y después de la llamada |
| Resultados | `benchmarks/pathfinder/results/ara-compare-<fecha>/` (ignorado por Git) |

Ara con VLEN 256 no es una configuración habitual del proyecto Ara (sus
configuraciones de referencia usan VLEN de 2048 bits o más), pero es
legal (`VLEN > 128`) y pasa la verificación completa.

## Método

### Mismo kernel en ambas plataformas

| Programa | Objetos comunes | Propio |
| --- | --- | --- |
| gem5 SE | `kernel.o`, `data.o`, `check.o` | `main.o`: `_start`, `write`/`exit` de Linux, m5ops |
| Ara | los mismos | `main_ara.o` y runtime de Ara: `crt0`, `printf`, `tohost` |

`tools/compare_kernel.py` compara `runVector` en `kernel.o` y en los
binarios: deben coincidir todos los bits salvo los campos de inmediato de
las 8 instrucciones con relocación (direcciones de `vectorResult` y
`pathfinderWall`). También exige el mismo desplazamiento de los datos en
la línea de 64 bytes. Si falla, `compare_ara.py` no simula. Se probó que
detecta un bit cambiado fuera de esos campos.

### Qué se mide

- **Ara:** traza FST del RTL (`ara/ara_trace.py`): secuenciador, unidades,
  lanes, bancos del VRF y AXI; y traza de retirada de CVA6.
- **gem5:** `stats.txt` de la región y trazas `Exec` (retirada) y
  `VectorTimeline` (entrega, inicio y fin de cada comando).

La traza de Ara se toma en una segunda ejecución sin la referencia
escalar, que cuesta varias veces el kernel. Esa ejecución debe dar la
misma huella y los mismos ciclos de kernel que la completa.

### Escenario `memAra`: calibración de la latencia

`riscv_vpu.py` admite ahora `--mem-latency`. En Ara, una lectura
vectorial de 32 bytes tarda 7 ciclos hasta el primer beat y 10 hasta el
último, y una escritura 22 ciclos hasta la respuesta B. Ponderado por
número de ráfagas (2 lecturas por cada escritura), son **14,0 ciclos por
bloque**. En gem5 (`tiny`, `vlsu.memoryTicks / vlsu.blocks`):

| `--mem-latency` | Ciclos por bloque | Ciclos del kernel |
| --- | --- | --- |
| 30 ns (defecto) | 44,7 | 42 872 |
| 5 ns | 19,7 | 26 007 |
| 2 ns | 16,7 | 23 996 |
| 1 ns | 15,7 | 23 327 |
| **0 ns** | **14,7** | 22 664 |

El resto (14,7 ciclos con latencia 0) es el coste fijo del `SystemXBar` y
del ancho de banda de `SimpleMemory`. `memAra` usa 0 ns. Como Minor no
tiene cachés, también acelera sus lecturas de instrucciones y sus loads.

## Comprobaciones de corrección

Todas se cumplen en `tiny` y en `small`:

| Comprobación | Qué descarta |
| --- | --- |
| `runVector` idéntico salvo direcciones; datos con el mismo desplazamiento | Comparar código o colocación distintos |
| `PASS` en Ara (con referencia escalar), en la ejecución trazada y en los cuatro escenarios de gem5; la misma huella en todos (`small`: `sum=188600 xor=46`) | Resultado incorrecto en alguna plataforma |
| Ara trazado: mismos ciclos de kernel que sin traza (1 376 338 en `small`) | Que la traza o la variante sin referencia escalar alteren el kernel |
| Mismas instrucciones retiradas en `runVector`, por mnemónico, en Ara y en gem5 (incluido native) | Regiones medidas distintas |
| Instrucciones aceptadas por Ara y comandos de gem5 = instrucciones vectoriales retiradas sin `vsetvli` (130 048 en `small`); línea temporal completa | Perder o duplicar trabajo vectorial |
| Mismos bytes leídos y escritos por la VPU (1 040 384 / 520 192 en `small`) | Distinto tráfico de memoria vectorial |
| Mismas lecturas y escrituras del VRF y elementos de ALU por lane (lane 0: 292 608 / 227 584 y 195 072) | Distinto trabajo en lanes y VRF |

Las dos últimas coinciden exactamente en palabras de 64 bits, aunque se
cuentan en señales RTL en un lado y en estadísticas del modelo en el
otro. Con 8 bancos por lane, además, el reparto por banco de gem5 es
idéntico al de Ara (lane 0: 162 560, 162 560, 32 512, 32 512, 65 024,
65 024, 0, 0): el mapeo `word_striped` coincide con el de Ara para este
kernel. Con VLEN 256 cada registro ocupa 2 de los 8 bancos y los
bancos 6 y 7 no se usan.

## Resultados

`small`, 16 256 tiras de 8 elementos (127 filas × 128 tiras):

| Plataforma | Ciclos del kernel | Ciclos por tira | Respecto a Ara |
| --- | --- | --- | --- |
| Ara RTL | 1 376 338 | 84,7 | 1,00 |
| gem5 `base` | 4 804 168 | 295,5 | 3,49 |
| gem5 `memAra` | 2 737 378 | 168,4 | 1,99 |
| gem5 `memAra+banks8` | 2 688 610 | 165,4 | 1,95 |
| gem5 `memAra+banks8+win8` | 2 688 610 | 165,4 | 1,95 |
| gem5 native (RVV en Minor, sin VPU) | 4 216 879 | 259,4 | 3,06 |

`tiny` da la misma proporción: 11 310 ciclos en Ara; 42 872, 22 664 y
22 292 en los tres escenarios (3,79, 2,00 y 1,97 veces Ara).

### Descomposición de la diferencia

Ciclos por tira en `small`:

| Paso | Ciclos por tira | Diferencia | Causa |
| --- | --- | --- | --- |
| gem5 `base` | 295,5 | — | — |
| → `memAra` | 168,4 | −127,1 | Memoria: −90 en `vle`/`vle`/`vse` (de 159 a 69), −31 en el `lw` escalar y −6 en el código fuera del bucle de tiras (cambio de fila y copia inicial escalar, sin caché) |
| → `memAra+banks8` | 165,4 | −3,0 | Conflictos de banco en el VRF: 1 ciclo menos en `vmin` y `vadd` |
| → Ara | 84,7 | −80,7 | **Microarquitectura: Ara solapa instrucciones; gem5 las ejecuta en serie** |

El 60 % de la diferencia de `base` (127,1 de 210,8 ciclos por tira) se
debe al modelo de memoria de `riscv_vpu.py`, que no representa la L2 de
Ara (44,7 ciclos por bloque frente a 14,0). Con la memoria igualada,
gem5 tarda el doble.

### Ventanas como las de Ara: sin efecto

`memAra+banks8+win8` sube a 8 la cola de comandos, los comandos activos,
los contextos por lane y la ventana vectorial de Minor (Ara admite 8
instrucciones en vuelo, `NrVInsn`). Da exactamente los mismos ciclos en
`tiny` (22 292) y `small` (2 688 610), y también en `base` (42 872 y
4 804 168). Nunca hay más de 2 comandos en la VPU ni más de 1
ejecutándose: Minor deja de despachar ante memoria, instrucciones
escalares y configuración, que Pathfinder intercala entre las
aritméticas, y la VPU serializa por dependencias y por la barrera de
memoria. En el hito 2 el límite son esas reglas, no el tamaño de las
ventanas.

### El resto: solapamiento

Por instrucción, gem5 y Ara tardan lo mismo. Medias en `small`, de la
entrega o petición al fin, en ciclos:

| Instrucción | Ara | gem5 `memAra+banks8` |
| --- | --- | --- |
| `vle32.v` (×2) | 18 | 22 |
| `vslide1up.vx` | 16 | 15 |
| `vmin.vv` (×2) | 15 | 15 |
| `vslide1down.vx` | 25 | 14 |
| `vadd.vv` | 17 | 29 |
| `vse32.v` | 27 | 24 |
| **Suma por tira** | **151** | **156** |

La diferencia es cómo se encadenan:

- **gem5:** con las reglas del hito 2 (sin chaining, memoria como barrera,
  scoreboard conservador) nunca hay dos comandos ejecutándose
  (`sequencer.peakOutstanding` = 1). Los 156 ciclos se suman en serie, y
  Minor retira cada instrucción vectorial al completarse. `vadd` espera 15
  ciclos a que termine el `vmin` del que depende.
- **Ara:** asigna cada instrucción al llegar y encadena los resultados.
  Hay 1,51 instrucciones en vuelo de media y hasta 4: los 151 ciclos de
  residencia caben en unos 83 por tira. CVA6 retira la instrucción
  vectorial al aceptarla Ara y sigue con el código escalar.

Costes propios de Ara, que gem5 no tiene o tiene menores:

| Coste en Ara | Ciclos por tira | Causa |
| --- | --- | --- |
| `lw` de `aux2` tras el `vse32.v` | 24,8 | El load escalar espera a que termine el store vectorial (AW→B: 22 ciclos). En gem5, `vse32.v` ya se retiró completado y el `lw` cuesta 10,7 |
| Entrada de `vslide1down.vx` | 14 | Una única unidad de slide: espera a que termine `vslide1up.vx` |
| `vsetvli` | 4 | Lo resuelve el dispatcher de Ara; en gem5, 0,3 |
| CVA6 esperando a que Ara acepte | 29,7 % de la ventana | El secuenciador aún no acepta la instrucción; la causa por instrucción no se ha desglosado |

### Recursos

| Métrica (`small`) | Ara | gem5 `memAra+banks8` | ¿Misma definición? |
| --- | --- | --- | --- |
| Ciclos con alguna instrucción en la VPU | 76,1 % | 85,9 % | Sí |
| Instrucciones en vuelo: media / máximo | 1,51 / 4 | 0,81 / 1 (ejecutándose) | No |
| Memoria ocupada | 30,9 % (máximo de load y store) | 37,5 % (VLSU) | No: Ara separa load y store |
| ALU de la lane 0 | 7,1 % de ciclos con resultado | 3,6 % (`alu0.busyTicks`) | No |

Las unidades están ociosas la mayor parte del tiempo en ambos lados: el
kernel es una cadena de dependencias con VL 8 y no da trabajo suficiente
a 2 lanes. Ninguna unidad aritmética limita.

## Conclusiones

1. **Se puede comparar.** El trabajo coincide exactamente (instrucciones,
   bytes, accesos al VRF y elementos de ALU) y el resultado también. La
   diferencia de ciclos se explica sin restos sin identificar.
2. **El modelo de memoria por defecto domina.** `SimpleMemory` de 30 ns sin
   cachés triplica la latencia por bloque de Ara y penaliza también al
   núcleo escalar. Para comparar con Ara hay que usar `memAra` o un
   sistema de memoria con cachés.
3. **La VPU de gem5 tarda lo mismo por instrucción; le falta el
   solapamiento.** Con memoria igualada la distancia es de 2 veces, y se
   corresponde con la ejecución en serie que impone el hito 2. Chaining
   y la concurrencia de memoria, previstos para el hito 3, son las
   palancas.
4. **Ara también tiene costes que gem5 no modela.** La espera del load
   escalar tras el store vectorial (unos 25 ciclos por tira) y la
   aceptación lenta de `vsetvli`. Si el hito 3 retira las instrucciones
   vectoriales antes de completarlas, gem5 deberá modelar esta espera.
5. **Validación del VRF.** Con 8 bancos, el reparto por banco coincide con
   el de Ara. Es la primera evidencia externa de que el mapeo del VRF es
   el de Ara.

## Límites

- El núcleo escalar no es el mismo: CVA6 con cachés L1 frente a Minor sin
  cachés. `memAra` iguala la latencia de memoria, no la jerarquía.
- No se ha ejecutado el escenario con cachés L1 en Minor (opcional en la
  propuesta). Requiere comprobar que los stores de la VPU invalidan la L1.
- Una carga y una configuración: VL 8, LMUL 1, 2 lanes. Con VLEN 4096 y
  4 lanes (la configuración por defecto de Ara) el kernel tiene otro
  equilibrio.
- Los ciclos de Ara incluyen el `fence` final; los de gem5, la retirada del
  último comando. La diferencia es de pocos ciclos.

## Cambios de esta entrega

| Archivo | Cambio |
| --- | --- |
| `benchmarks/pathfinder/src/` | `pathfinder.c` se divide en `kernel.c`, `data.c`, `check.c`, `main.c` y `pathfinder.h` |
| `benchmarks/pathfinder/Makefile` | Objetos separados, `-mcmodel=medany -mno-relax`, `ALIGN`, objetivo `shared` |
| `benchmarks/pathfinder/tools/embed_input.py` | Declaraciones en la cabecera; definiciones sólo con `PATHFINDER_DEFINE_INPUT`; `PATHFINDER_DATA_ALIGN` |
| `benchmarks/pathfinder/tools/compare_kernel.py` | Nuevo: kernel idéntico y alineamiento |
| `benchmarks/pathfinder/ara/` | Nuevo: `Makefile`, `main_ara.c`, `run_ara.py`, `ara_trace.py`, `README.md` |
| `benchmarks/pathfinder/compare_ara.py` | Nuevo: comparación completa |
| `configs/example/riscv_vpu.py` | `--mem-latency` |
| `src/cpu/vector_engine/` | Flag `VectorTimeline` en `SConscript`; trazas en `VectorEngine::dispatch`, `completed` y `AraSequencer::start`. Sin cambios de temporización: el binario anterior reproduce sus ticks |

La división del fuente cambia la colocación del código y de los datos sin
alinear. Por eso `run.py` (sin `ALIGN`) da otros ticks que antes (ver el
[README del benchmark][benchmark]). Los datos de
[Observabilidad - análisis con Pathfinder][obs] corresponden al binario
anterior.

## Reproducción

```bash
make -C <ara>/hardware verilate trace=1 nr_lanes=2 vlen=256 \
    buildpath=build_2l_v256
python3 -m venv ~/venvs/ara-trace
~/venvs/ara-trace/bin/pip install pywellen numpy
python3 benchmarks/pathfinder/compare_ara.py --input small \
    --trace-python ~/venvs/ara-trace/bin/python
```

Requisitos y detalles en [`benchmarks/pathfinder/ara/README.md`][ara].

[benchmark]: <../../benchmarks/pathfinder/README.md>
[ara]: <../../benchmarks/pathfinder/ara/README.md>
[plan]: <../Plan de evolución - Hitos 2 a 5.md>
[bench]: <../Plan de benchmarks.md>
[obs]: <Observabilidad - análisis con Pathfinder.md>
