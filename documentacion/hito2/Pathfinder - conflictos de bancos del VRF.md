# Pathfinder: conflictos de bancos del VRF en la VPU y en Ara

Repetimos en la VPU de gem5 el estudio de conflictos de bancos del VRF que
se hizo en Ara RTL. El análisis de Ara está en
`ara/hardware/vrf_bank_conflicts/pathfinder_r7_c512/SECTION_4.md`. Usamos el
mismo kernel, los mismos datos y las mismas métricas. Al final comparamos las
dos máquinas.

## Resumen

| | Ara RTL | VPU de gem5 (8 bancos) |
| --- | --- | --- |
| Ciclos de la ventana medida | 2.382 | 13.951 |
| Accesos por banco y lane | 696 (400 L + 296 E) | 696 (400 L + 296 E) |
| Ciclos con conflicto | 482 (20,24 %) | 1.152 (8,26 %) |
| Dentro de una instrucción / entre instrucciones | 114 / 368 | 1.152 / 0 |
| Bancos activos a la vez (máximo) | 5 | 2 |
| Ciclos perdidos por conflictos | ≈ 0 | **≈ 1.134 (8,1 %)** |

- **El trabajo en el VRF es idéntico:** las dos máquinas hacen los mismos
  5.568 accesos por lane, repartidos igual entre los 8 bancos.
- **En gem5 el porcentaje de ciclos con conflicto es menor,** porque las
  instrucciones no se solapan y la ventana es casi seis veces más larga.
- **Pero en gem5 cada conflicto cuesta un ciclo, y en Ara casi ninguno.**
  En gem5, cada palabra de cada instrucción de ALU pide sus dos operandos al
  mismo banco en el mismo ciclo (1.152 = 18 instrucciones × 64 palabras).
  Como la ALU está en el camino crítico, cada conflicto alarga el kernel un
  ciclo. Si se eliminan los conflictos, el kernel tarda 1.134 ciclos menos.

## Método

| Aspecto | Cómo |
| --- | --- |
| Kernel y datos | `kernel/pathfinder.c.o` y `data.S.o` compilados por Ara en `ara/apps/pathfinder_r7_c512`, enlazados sin recompilar en el programa de gem5 (`benchmarks/vrf_bank_conflicts/main_gem5.c`). Los 75 encodings de `run_vector_short_m4` son idénticos en los dos binarios. |
| Alineamiento | `wall` y `result_v` tienen el mismo desplazamiento dentro de la página y de la línea que en Ara (`...170` y `...5d0`). |
| Comprobación | `512/512 resultados correctos` frente a la referencia de NumPy, en todas las ejecuciones. |
| Geometría | `--vlen 4096`, `vrf.num_lanes=4`, `vrf.banks_per_lane=8`, un puerto RW por banco y `supported_lmuls=0,2` para habilitar m4. Es la geometría de Ara. |
| Memoria | `--mem-latency=0ns`, el escenario `memAra` calibrado en [la comparación con Ara](<Pathfinder - comparación con Ara RTL.md>). |
| Ventana | Entre las m5ops `reset_stats` y `dump_stats` alrededor de la llamada al kernel. Es el equivalente de `hw_cnt_en_o[0]` en Ara. |
| Observación | Flag nuevo `VrfArbitration` en `LaneRegisterFile::arbitrate()`: una línea por candidato elegible y ciclo, con banco, fila, sentido, peticionario, comando y si se le concedió el acceso. |
| Métricas | `benchmarks/vrf_bank_conflicts/analyze_gem5_vrf.py`, con las mismas definiciones que el script de Ara: un banco está en conflicto si lo piden dos o más candidatos en el mismo ciclo, y un ciclo con conflicto cuenta una vez. |

Los peticionarios de cada máquina se corresponden así:

| VPU de gem5 | Ara |
| --- | --- |
| Lane, `Lhs` / `Rhs` | `AluA` / `AluB` |
| Lane, `Destination` | Escritura ALU |
| VLSU, escritura / lectura | Escritura de load / `StA` |
| SLDU, lectura / escritura | `SlideAddrGenA` / escritura de slide |

## Resultados (lane 0; las cuatro lanes son idénticas)

| Métrica | Resultado |
| --- | --- |
| Ciclos de la ventana | 13.951 (kernel: 13.950 según las estadísticas) |
| Ciclos con conflicto | 1.152 |
| Porcentaje con conflicto | 8,26 % |
| Ciclos con conflicto en dos bancos | 0 |
| Accesos por banco | 696 en cada uno (400 lecturas + 296 escrituras) |

| Bancos activos | Ciclos |
| --- | --- |
| 0 | 9.517 |
| 1 | 3.300 |
| 2 | 1.134 |
| 3–8 | 0 |

| Peticionario | Peticiones | Accesos | Denegadas |
| --- | --- | --- | --- |
| `Lhs` (AluA) | 1.152 | 1.152 | 0 |
| `Rhs` (AluB) | 2.304 | 1.152 | **1.152** |
| `Destination` (escritura ALU) | 1.152 | 1.152 | 0 |
| SLDU, lectura | 768 | 768 | 0 |
| SLDU, escritura | 768 | 768 | 0 |
| VLSU, escritura (load) | 448 | 448 | 0 |
| VLSU, lectura (store) | 128 | 128 | 0 |

Todos los conflictos son del mismo tipo: `Lhs` contra `Rhs` de una misma
`vmin.vv` o `vadd.vv`. No hay ningún conflicto entre instrucciones distintas.

### Ejemplo

Primer conflicto: ciclo 1.982 (el 1.928 de la ventana), lane 0, banco 0,
`vmin.vv v28, v16, v12`:

| Peticionario | Registro | Palabra | Fila del banco | Resultado |
| --- | --- | --- | --- | --- |
| `Lhs` | v16 | 256 | 32 | Concedido |
| `Rhs` | v12 | 192 | 24 | Denegado; lee en el ciclo siguiente |

Cuatro ciclos después se repite lo mismo con las palabras 257 y 193 en el
banco 1, y así con todas las palabras de la instrucción.

## Por qué todos los conflictos están dentro de una instrucción

**Mapeo de registros a bancos.** Con VLEN = 4096 y 4 lanes, cada registro
ocupa 16 palabras por lane. La política `word_striped` coloca la palabra
local `k` en el banco `k mod 8`. Como 16 es múltiplo de 8,
**todo registro empieza en el banco 0**, igual que en Ara.

**Lectura de operandos por bundles.** La lane lee los dos operandos de cada
bundle (una palabra) a la vez: la palabra `k` de `vs2` y la palabra `k` de
`vs1`, que están en el mismo banco. El árbitro concede `Lhs` y `Rhs` pasa al
ciclo siguiente. Con `operand_entries_per_source = 1` y un solo contexto activo
por instrucción, la lane no puede adelantar la lectura del bundle siguiente.
Por eso los dos operandos vuelven a ir juntos en cada palabra y chocan
**en todas las palabras**. En Ara, en cambio, las colas `AluA` y `AluB` son
independientes: tras un conflicto se desfasan un ciclo y dejan de chocar.

**Las instrucciones no se solapan.** La VPU del hito 2 no encadena
(dependencia RAW = esperar a que el productor termine) y la memoria actúa como
barrera. En Pathfinder, cada instrucción depende de la anterior o es una
operación de memoria, así que nunca hay dos instrucciones accediendo al VRF a
la vez y no puede haber conflictos entre instrucciones. Ensanchar las ventanas
a 8 no cambia nada (13.950 ciclos y los mismos 1.152 conflictos), igual que en
la comparación de ciclos.

## ¿Cuesta tiempo?

En gem5 sí. Para medirlo, repetimos la ejecución cambiando un recurso cada vez:

| Escenario | Ciclos del kernel | Conflictos | `Rhs` denegadas | `vmin`/`vadd` (ciclos) | Fila (ciclos) |
| --- | --- | --- | --- | --- | --- |
| 8 bancos, 1 puerto (geometría de Ara) | 13.950 | 1.152 | 1.152 | 263 | 1.903 – 2.564 |
| 2 bancos (predeterminado del hito 2) | 13.950 | 1.152 | 1.152 | 263 | igual |
| 8 bancos y ventanas de 8 | 13.950 | 1.152 | 1.152 | 263 | igual |
| 8 bancos, **2 puertos por banco** | **12.816** (−1.134) | 1.152 | **0** | 200 | 1.714 – 2.375 |
| 8 bancos, 2 entradas de operandos y de resultados en la ALU | 11.952 (−1.998) | 1.134 | 1.152 | 152 | 1.570 – 2.231 |

- **Con dos puertos por banco desaparece la espera,** aunque los dos accesos
  sigan cayendo en el mismo banco. Cada instrucción de ALU pasa de 263 a 200
  ciclos (−63 = 64 palabras − 1) y el kernel ahorra
  18 × 63 = **1.134 ciclos, un 8,1 %**. Ese es el coste exacto de los conflictos.
- **Con 2 bancos tarda lo mismo que con 8:** como solo hay un acceso
  doble por palabra, más bancos no ayudan si los dos operandos siguen en fase.
- **Con más buffers en la ALU, el conflicto queda parcialmente oculto:** la
  ALU solapa la lectura del bundle siguiente con el cálculo del actual. El
  conflicto sigue ocurriendo, pero cuesta menos.

En Ara, en cambio, el coste es prácticamente nulo. Las filas duran 337–339
ciclos tanto si tienen 0 conflictos como si tienen 120, porque el ritmo lo
marca la unidad de slides (ver `SECTION_4.md`).

## Comparación con Ara

| | Ara RTL | VPU de gem5 |
| --- | --- | --- |
| Duración de una fila | 337–339 ciclos | 1.903–2.564 ciclos |
| `vle32.v` (64 palabras por lane) | 141 ciclos, solapado con el cálculo | 660 ciclos, barrera |
| `vslide1up` / `vslide1down` | 136 / ~127 ciclos efectivos, encadenados con la `vmin` | 215 / 227 ciclos, sin encadenar |
| `vmin.vv` / `vadd.vv` | Siguen al productor con encadenamiento | 263 ciclos cada una (≈4 por palabra) |
| Instrucciones a la vez en el VRF | Hasta 4–5 unidades: ALU, slide, load y store | Una |
| Conflictos dentro de una instrucción | 114, solo cuando dos operandos se resincronizan | 1.152, en todas las palabras |
| Conflictos entre instrucciones | 368 (slide contra la `vmin` que la consume, precarga contra la ALU) | 0 |
| Prioridad del árbitro | Fija: la ALU antes que load, slide y store | Round-robin por entrada; el más antiguo primero si hay riesgo de fila |
| Coste en tiempo | ≈ 0, oculto en el ritmo de la SLDU | 1 ciclo por conflicto (8,1 % del kernel) |

**Conclusiones para los hitos siguientes:**

1. **Desacoplar la lectura de los dos operandos** elimina por completo el
   coste actual: colas por operando con más de una entrada, como `AluA` y
   `AluB` en Ara, o la lectura adelantada de bundles. Vale en cualquier
   geometría, porque todos los registros empiezan en el banco 0.
2. Al añadir encadenamiento y solape (hito 3), aparecerán los **conflictos
   entre instrucciones** que ya se ven en Ara: slide contra consumidor y
   precarga contra escritura de la ALU. Para que entonces sigan sin costar
   tiempo, el árbitro debe favorecer el camino crítico, como hace la prioridad
   de Ara, y las unidades deben poder reintentar sin perder su posición.
3. El 20 % de conflictos de Ara y el 8 % de gem5 **no son comparables** como
   medida de pérdida. El indicador útil es el tiempo que cuestan, medido como
   aquí con un escenario sin conflictos.

## Reproducción

```bash
make -C benchmarks/vrf_bank_conflicts          # necesita los .o de ara/apps/pathfinder_r7_c512
build/RISCV/gem5.opt -re --outdir=benchmarks/vrf_bank_conflicts/results/banks8 \
    --debug-flags=VrfArbitration,VectorTimeline,Exec --debug-file=trace.out \
    configs/example/riscv_vpu.py --vlen 4096 --vpu-set vrf.num_lanes=4 \
    --vpu-set supported_lmuls=0,2 --vpu-set vrf.banks_per_lane=8 --mem-latency=0ns \
    benchmarks/vrf_bank_conflicts/build/pathfinder_r7_c512
python3 benchmarks/vrf_bank_conflicts/analyze_gem5_vrf.py \
    benchmarks/vrf_bank_conflicts/results/banks8 \
    --dump benchmarks/vrf_bank_conflicts/build/pathfinder_r7_c512.dump
```

Los demás escenarios añaden `--vpu-set bank.read_write_ports=2
--vpu-set bank.access_entries=2`, `--vpu-alu-operand-entries 2
--vpu-alu-result-entries 2` o las opciones de ventana de 8. Para el de 2
bancos se omite `banks_per_lane` y se pasa `--banks 2` al script. Los
detalles están en el README del benchmark.

## Cambios en gem5 para este estudio

- `configs/example/riscv_vpu.py`: opción `--vlen`, que fija VLEN en la ISA y
  en el VRF de la VPU. Antes estaba fijado a 256.
- `src/cpu/vector_engine/SConscript`: flag `VrfArbitration`.
- `src/cpu/vector_engine/vpu/register_file/lane_register_file.cc`: una línea
  `VrfArbitration` por candidato elegible tras el arbitraje. No cambia la
  temporización.
