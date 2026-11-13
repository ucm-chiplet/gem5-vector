# Pathfinder en Ara RTL: contraste con la VPU de gem5

Herramientas para ejecutar **el mismo kernel** de Pathfinder en Ara RTL
(Verilator) y en la VPU de gem5, comprobar que hacen el mismo trabajo y
comparar su rendimiento. El análisis y los resultados están en
[Pathfinder - comparación con Ara RTL][doc].

No es una calibración: gem5 no se ajusta a Ara. Sirve para descomponer
la diferencia de ciclos en causas identificadas (memoria, VRF, núcleo
escalar, microarquitectura de la VPU).

## Cómo se garantiza que es el mismo kernel

`../Makefile` compila con Clang `kernel.o` (`runVector`), `data.o`
(entrada y referencia) y `check.o` (referencia escalar y comprobaciones).
Los dos programas enlazan **esos mismos objetos**:

| Programa | Arranque y salida | Región medida |
| --- | --- | --- |
| gem5 (`../src/main.c`) | `_start` propio y llamadas Linux `write`/`exit` | `m5_reset_stats`/`m5_dump_stats` |
| Ara (`main_ara.c`) | Runtime bare-metal de Ara: `crt0`, `printf` por la UART simulada, `tohost` | CSR `cycle` (tras `fence`) alrededor de `runVector()` |

`-mcmodel=medany` permite cargar el kernel en `0x80000000` (Ara) y en
`0x10000` (gem5); `-mno-relax` evita que el enlazador reescriba sus
instrucciones. `../tools/compare_kernel.py` comprueba que `runVector`
tiene los mismos bytes en `kernel.o` y en los binarios, salvo los campos
de inmediato de las instrucciones con relocación, y que los datos tienen
el mismo desplazamiento dentro de la línea de 64 bytes (`ALIGN=64`).

## Requisitos

1. **Ara** compilado (`make toolchain-llvm verilator` en la raíz de Ara y
   `make checkout apply-patches` en `hardware/`). Con LLVM 20,
   `toolchain-llvm-rt` necesita desactivar `CTX_PROFILE`, `GWP_ASAN` y
   `ORC` de compiler-rt; si no, falla con
   `"This operating system is not supported"`.
2. **Modelo de Verilator** con la geometría de la VPU de gem5 (2 lanes,
   VLEN 256) y traza:

   ```bash
   make -C <ara>/hardware verilate trace=1 nr_lanes=2 vlen=256 \
       buildpath=build_2l_v256
   ```

3. **Python con `pywellen` y `numpy`** para leer la traza FST, por
   ejemplo en un entorno virtual:

   ```bash
   python3 -m venv ~/venvs/ara-trace
   ~/venvs/ara-trace/bin/pip install pywellen numpy
   ```

4. `build/RISCV/gem5.opt` compilado (incluye el flag `VectorTimeline`).
   `matplotlib` en el Python del sistema, opcional, para las figuras.

Por defecto Ara se busca en `../../../../ara` respecto a esta carpeta
(`ARA_ROOT` en make, `--ara-root` en los scripts).

## Uso

```bash
python3 benchmarks/pathfinder/compare_ara.py --input tiny \
    --trace-python ~/venvs/ara-trace/bin/python
python3 benchmarks/pathfinder/compare_ara.py --input small \
    --trace-python ~/venvs/ara-trace/bin/python
```

`compare_ara.py` compila, comprueba el kernel (si difiere no simula),
simula Ara y los escenarios de gem5 en paralelo y escribe
`../results/ara-compare-<fecha>/summary.md`. `small` tarda unos 10
minutos, casi todo en Verilator. Otras opciones: `--nr-lanes`/`--vlen`
para otro modelo de Ara, `--simulator`, `--keep-trace`, `--no-native`.

Por separado:

```bash
make -C benchmarks/pathfinder/ara INPUT=small check   # compila y compara
python3 benchmarks/pathfinder/ara/run_ara.py --input small \
    --trace-python ~/venvs/ara-trace/bin/python --out /tmp/ara-small
```

## Escenarios de gem5

| Escenario | Opciones de `riscv_vpu.py` | Qué aísla |
| --- | --- | --- |
| `base` | — | El modelo tal cual: `SimpleMemory` de 30 ns, sin cachés |
| `memAra` | `--mem-latency=0ns` | Latencia de memoria: 14,7 ciclos por bloque frente a 14,0 en Ara |
| `memAra+banks8` | además `--vpu-set=vrf.banks_per_lane=8` | Los 8 bancos por lane de Ara |
| `memAra+banks8+win8` | además cola, comandos activos, contextos por lane y ventana de Minor a 8 | Las 8 instrucciones en vuelo de Ara |
| `native` | `--no-offload` | Referencia: RVV ejecutado en Minor, sin VPU |

`memAra` cambia la latencia de toda la memoria, también la de las
lecturas de instrucciones y los loads de Minor, que no tiene cachés. Su
calibración está en el documento de la comparación.

## Qué se mide en Ara

`ara_trace.py` lee la traza FST (convención de muestreo en su
docstring). Señales, bajo
`TOP.ara_tb_verilator.dut.i_ara_soc.i_system.i_ara`:

| Métrica | Señales |
| --- | --- |
| Instrucciones aceptadas y espera de CVA6 | `i_sequencer.ara_req_valid_i`, `ara_req_ready_o` |
| Instrucciones en vuelo y línea temporal | `i_sequencer.vinsn_running_q` (un bit por identificador) |
| Ocupación por unidad | `i_sequencer.pe_vinsn_running_q` (lanes, load, store, máscaras, slide) |
| ALU, slide, load y store por lane | `alu_result_req/gnt`, `sldu_*`, `ldu_result_*`, `stu_operand_*` de `gen_lanes[i].i_lane` |
| VRF | `gen_lanes[i].i_lane.i_vrf.req_i` y `wen_i` (un bit por banco) |
| Memoria | `i_vlsu.i_addrgen.axi_ar/aw_*`, `i_vldu.axi_r_*`, `i_vstu.axi_w_*`, `axi_b_*` |

La traza de retirada de CVA6 (`trace_hart_0.dasm`) da el ciclo de
retirada de cada instrucción de `runVector`. En gem5 se obtiene lo mismo
con `--debug-flags=Exec`, y la entrega, el inicio y el fin de cada
comando con `VectorTimeline`.

## Archivos

| Archivo | Función |
| --- | --- |
| `Makefile` | Runtime de Ara, `main_ara.c` y enlace con los objetos comunes; `check` |
| `main_ara.c` | Programa bare-metal; `PATHFINDER_SKIP_SCALAR=1` omite la referencia escalar en la ejecución trazada |
| `run_ara.py` | Compila, simula (`full` y `traced`) y extrae métricas |
| `ara_trace.py` | Métricas de la traza FST |
| `../compare_ara.py` | Orquesta Ara y gem5, comprueba y genera el informe |
| `../tools/compare_kernel.py` | Kernel idéntico y alineamiento de datos |

[doc]: <../../../documentacion/hito2/Pathfinder - comparación con Ara RTL.md>
