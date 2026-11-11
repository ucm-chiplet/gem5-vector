# Fase 6 del hito 2: FPU FP32, operaciones enteras y varias ALUs, entrega VPU

Implementación autorizada por el usuario el 5 de noviembre de 2026, sobre
`f0566a6ae4`, con el alcance de la propuesta presentada en la conversación
y sus decisiones: el signo de división y resto viaja en `signedness`, cada
clase de unidad tiene sus propios buffers (como las colas por unidad de
Ara), no se modifica Minor y se adoptan los perfiles temporales propuestos.
La entrega atiende O2 (unidades con ocupación y perfiles distintos,
divisores no segmentados y créditos por clase) y O6 (ocupación por
instancia, esperas por clase, flags y matriz de soporte).

## Alcance y estado de la evidencia

La VPU sabe calcular, temporizar y cerrar:

- `vmul` (bits bajos), `vdiv`, `vdivu`, `vrem` y `vremu` en formas
  `.vv` y `.vx`, con SEW 8/16/32/64, en la ALU.
- `vfadd.vv`, `vfmul.vv` y `vfdiv.vv` FP32 en la FPU, con el redondeo
  capturado en el comando y los flags agregados hasta `VectorCompletion`.
- Varias instancias de ALU por lane (`alus_per_lane`).

**Frontera con Minor.** No se modifica la CPU. Minor sigue decodificando
sólo `vadd.vv/.vx`, `vle32.v` y `vse32.v`; cualquier otra instrucción
vectorial recibe `IllegalInstFault` en decode. Por tanto, **ningún programa
alcanza todavía las operaciones nuevas** y los resultados y tiempos de los
programas actuales no cambian por esta entrega, salvo por el cambio de
nombres de parámetros y estadísticas. Los pendientes de CPU se recogen en
la [sección 2.6 de los pendientes de Minor][pendientes-minor]. El
criterio 4 de la [sección 15.3][cierre] (recorrido CPU--FPU--VRF--CPU) y
el 13 (Mul/Div/resto mediante la ALU) quedan pendientes de esa parte.

**Estado posterior (11 de noviembre de 2026).** La rama de Minor,
integrada en `feature/vpu` hasta `963e6a9550`, decodifica `vmul`,
`vdiv`, `vdivu`, `vrem` y `vremu` en `.vv` y `.vx`, y `vfadd.vv`,
`vfmul.vv` y `vfdiv.vv` (`e27304868a`). Antes de reservar comprueba
`misa.V` y `mstatus.VS`, y `mstatus.FS` en las operaciones FP; al retirar
con éxito marca `VS` (y `FS` si procede) como `Dirty`. Verificado en
código, sin ejecución registrada: los criterios 4 y 13 siguen pendientes
de evidencia desde programa. La columna "Decode Minor" de la tabla
siguiente refleja este estado.

Compilar esta entrega no acredita ejecución funcional ni validación
temporal. No se han ejecutado cargas ni se han creado pruebas.

## Archivos

Bajo `src/cpu/vector_engine/`:

| Archivos | Responsabilidad |
| --- | --- |
| `common/vector_types.hh` | Operaciones `Multiply`, `Divide`, `Remainder`, `FloatMultiply` y `FloatDivide`; papel de operandos y signo. |
| `common/operation_traits.hh/.cc` | Fichas nuevas, signo requerido y anchos admitidos desde CPU (`offloadWidths`). |
| `common/vector_support.hh/.cc` | Validación del signo por ficha y admisión desde CPU por ancho. |
| `common/execution_types.hh` | `signedness` en `ExecutionBundle`. |
| `common/vpu_parameters.hh` | `OperationTiming`, `AluParameters`, `FpuParameters` y buffers por clase. |
| `vpu/lanes/alu.hh/.cc` | Mul/Div/resto, perfil por operación y entrega por `readyAt`. |
| `vpu/lanes/fpu.hh/.cc` | FP32, pipeline de suma/multiplicación y divisor no segmentado. |
| `vpu/lanes/ara_lane.hh/.cc` | Instancias de ALU, clases de unidad, buffers y emisión por clase. |
| `vector_engine.cc` | Construcción y validación de los parámetros nuevos. |
| `VectorEngine.py`, `configuration.py` | Parámetros, `AluConfiguration` y `FpuConfiguration`. |

Fuera de la VPU sólo cambia `configs/example/riscv_vpu.py`. No se modifican
Minor, `src/arch/`, `interface/`, `frontend/`, VLSU, backend de memoria,
VRF, `future/` ni `build/`.

## Operaciones y matriz de soporte

`VectorSupport` sigue siendo la autoridad. **Backend** indica que la VPU
calcula y temporiza la combinación; **admisión CPU** que `checkCommand` la
acepta en un comando; **decode Minor** que un programa puede generarla.

| Operación | Formas | Anchos backend | Admisión CPU | Unidad | Decode Minor |
| --- | --- | --- | --- | --- | --- |
| `vadd` (`Add`) | `.vv`, `.vx` | 8/16/32/64 | 8/16/32/64 | ALU | Sí |
| `vmul` (`Multiply`) | `.vv`, `.vx` | 8/16/32/64 | 8/16/32/64 | ALU | Sí (`e27304868a`) |
| `vdiv`, `vdivu` (`Divide`) | `.vv`, `.vx` | 8/16/32/64 | 8/16/32/64 | ALU | Sí (`e27304868a`) |
| `vrem`, `vremu` (`Remainder`) | `.vv`, `.vx` | 8/16/32/64 | 8/16/32/64 | ALU | Sí (`e27304868a`) |
| `vfadd.vv` (`FloatAdd`) | `.vv` | 16 interno, 32 | 32 | FPU | Sí (`e27304868a`) |
| `vfmul.vv` (`FloatMultiply`) | `.vv` | 32 | 32 | FPU | Sí (`e27304868a`) |
| `vfdiv.vv` (`FloatDivide`) | `.vv` | 32 | 32 | FPU | Sí (`e27304868a`) |

Todas las combinaciones siguen sujetas al perfil SEW/LMUL habilitado
(`supported_sews`, `supported_lmuls`), a la legalidad de grupos y a
`vm=1`. La suma binary16 conserva su carácter interno: `offloadWidths` la
excluye y admisión rechaza un `FloatAdd` de 16 bits desde CPU.

### Papel de los operandos

RVV define `vd[i] = vs2[i] op vs1[i]` (o `op rs1`). El contrato fija:

| Campo | Registro RVV | En el bundle |
| --- | --- | --- |
| `ArithmeticCommand::vectorSource` | `vs2` (dividendo en `vdiv`/`vrem`/`vfdiv`) | `lhs` |
| `ArithmeticCommand::secondOperand` | `vs1` o `rs1` normalizado | `rhs` |

**Discrepancia con Minor.** El decode actual coloca `vs1` en
`vectorSource` en las formas `.vv`, porque en gem5 `srcRegIdx(0)` es `vs1`.
Es inocuo para `vadd`, que es conmutativa, pero invertiría división y resto.
Debe corregirse antes de decodificarlas (pendiente 2 de Minor 2.6).

### Signo

`ElementSignedness` distingue las variantes: `Divide`/`Remainder` exigen
`Signed` (`vdiv`, `vrem`) o `Unsigned` (`vdivu`, `vremu`); `Add`,
`Multiply` y las FP exigen `NotApplicable`. Un signo que no corresponde a
una operación conocida se rechaza como `InvalidPayload` en
`checkStructure`. La multiplicación de bits bajos no depende del signo.

### Semántica entera

| Caso | Resultado |
| --- | --- |
| `vmul` | SEW bits bajos del producto (aritmética `uint64_t` módulo 2^64). |
| `vdiv`, `vrem` | Cociente truncado hacia cero y resto con el signo del dividendo. |
| División entre cero | Cociente con todos los bits a uno; el resto conserva el dividendo. |
| Overflow con signo (mínimo / −1) | Cociente igual al dividendo; resto cero. |

El overflow se resuelve antes de dividir, de modo que SEW=64 no produce
comportamiento indefinido en el host. La extensión de signo evita
convertir a `int64_t` valores mayores que `INT64_MAX` (C++17).

### Semántica FP32

- SoftFloat `f32_add`, `f32_mul` y `f32_div`, con el modo de redondeo
  del contexto capturado y detección de tininess tras redondear.
- Todo resultado NaN se sustituye por el NaN canónico `0x7fc00000`
  (binary16: `0x7e00`).
- Los flags de cada bundle son el OR de sus elementos, todos dentro del
  rango ejecutado; la lane los agrega por fragmento, el distribuidor por
  tarea y el sequencer por comando. Minor los acumula al retirar.
- Subnormales y ceros con signo siguen IEEE 754, sin flush.

## Organización y temporización

### ALU

Cada instancia admite suma, multiplicación, división y resto. Suma y
multiplicación son segmentadas; división y resto usan un divisor no
segmentado, que bloquea nuevas admisiones de esa instancia hasta su
resultado. Como las latencias difieren, los resultados salen por
`readyAt` y, a igualdad, en orden de admisión; la lane los localiza por
clave.

### FPU

Dos subrecursos independientes:

- Pipeline de suma/multiplicación, compartido también por la suma binary16,
  con `fpu_capacity` entradas e intervalo propio.
- Divisor FP32 no segmentado, con un bundle como máximo.

Una división en curso no rechaza sumas ni multiplicaciones FP, y la FPU
no comparte capacidad con las ALUs. No se modela una subunidad por
operación ni se interpone un único pipeline para todas.

### Perfiles por defecto

Son parámetros del modelo, en ciclos de la VPU; no son latencias
verificadas de Ara.

| Operación | Latencia | Intervalo | Recurso |
| --- | --- | --- | --- |
| `vadd` | 1 | 1 | ALU |
| `vmul` | 3 | 1 | ALU |
| `vdiv`, `vrem` (y sin signo) | 10 / 18 / 34 / 66 (SEW 8/16/32/64) | = latencia | Divisor de la ALU |
| `vfadd`, `vfmul` FP32 y suma binary16 | 4 | 1 | Pipeline FP |
| `vfdiv` FP32 | 12 | = latencia | Divisor FP |

### Lane: clases de unidad, buffers y emisión

Cada fragmento se asigna al admitirse a la clase de su ficha (ALU o FPU).

- **Buffers por clase.** Las parejas de operandos y los resultados se
  reservan del juego de su clase. Una división FP que espera al divisor
  no retiene créditos de las ALUs, y al revés.
- **Emisión.** Cada ciclo se inicia como máximo un bundle por clase, en
  orden de admisión dentro de la clase. Un fragmento joven no adelanta a
  otro de su clase que aún reúne operandos, pero sí a uno de la otra. El
  inicio de instrucciones sigue siendo FIFO en el sequencer.
- **Varias ALUs.** El bundle va a la instancia de menor índice que lo
  acepte. Cada instancia tiene su ocupación, capacidad e intervalo.
  Contextos (`lane_task_entries`), puertos del VRF y writeback son
  compartidos: más ALUs no crean operandos ni puertos.
- **Ancho.** `alu_width_bytes` y `fpu_width_bytes` siguen iguales a
  `lane_word_bytes`. Número de instancias y ancho SIMD son parámetros
  distintos; un ancho menor que la palabra no forma parte de esta entrega.

Con sólo trabajo entero y los valores por defecto, la emisión coincide con
la de la fase 5. Los contextos de lane siguen siendo compartidos: una
instrucción FP con muchos fragmentos puede ocuparlos y retrasar fragmentos
enteros de una tarea posterior. Es una limitación medida, no una garantía
de independencia.

### Diferencias con Ara

- En Ara, multiplicación, división y resto enteros se ejecutan en la
  VMFPU, junto a la FPU. El proyecto los asigna provisionalmente a la ALU
  (§8.3 y §11.3 del [diseño][diseno]); cambiar la asignación sólo requeriría
  modificar la clase de ejecución de sus fichas.
- Ara tiene una VALU por lane. Las instancias de ALU de este modelo
  comparten los buffers de su clase; no hay referencia de Ara para ello.
- Los buffers separados por clase sí siguen a Ara: cada unidad funcional
  tiene sus colas de operandos y de resultados.

## Parámetros

| Parámetro | Por defecto | Unidad y condición |
| --- | --- | --- |
| `alus_per_lane` | 1 | Instancias de ALU por lane; positivo. |
| `alu_capacity` | 1 | Bundles pendientes por instancia. |
| `alu_operand_entries_per_source` | 1 | Buffers de la clase ALU por fuente y lane. |
| `alu_result_entries` | 1 | Resultados de la clase ALU por lane, hasta `WriteAck`. |
| `alu_add_latency`, `alu_add_initiation_interval` | 1, 1 | Ciclos. |
| `alu_multiply_latency`, `alu_multiply_initiation_interval` | 3, 1 | Ciclos. |
| `alu_divide_latencies` | 10, 18, 34, 66 | Ciclos por SEW 8/16/32/64; exactamente cuatro valores. |
| `fpu_capacity` | 1 | Bundles en el pipeline de suma/multiplicación. |
| `fpu_operand_entries_per_source` | 1 | Buffers de la clase FPU por fuente y lane. |
| `fpu_result_entries` | 1 | Resultados de la clase FPU por lane, hasta `WriteAck`. |
| `fpu_add_multiply_latency`, `fpu_add_multiply_initiation_interval` | 4, 1 | Ciclos. |
| `fpu_divide_latency` | 12 | Ciclos; el divisor admite un bundle cada vez. |

Se eliminan `operand_buffer_entries_per_source` y `result_buffer_entries`
(sustituidos por los de cada clase), `alu_latency` y
`alu_initiation_interval` (ahora `alu_add_*`), y `fpu_latency` y
`fpu_initiation_interval` (ahora `fpu_add_multiply_*`). En
`configuration.py`, `LaneConfiguration` pierde sus buffers y recibe
`AluConfiguration` y `FpuConfiguration`. La clase FPU añade buffers que no
existían por separado, de modo que el total por lane aumenta.

`configs/example/riscv_vpu.py` añade `--vpu-alus-per-lane`,
`--vpu-alu-multiply-latency`, `--vpu-alu-divide-latencies`,
`--vpu-fpu-add-multiply-latency`, `--vpu-fpu-divide-latency` y los cuatro
`--vpu-{alu,fpu}-{operand,result}-entries`.

## Estadísticas

Cambios de nombres respecto a la fase 5:

| Antes | Ahora |
| --- | --- |
| `laneN.alu.*` | `laneN.alu0.*`, `laneN.alu1.*`, ... |
| `laneN.lhsBuffers`, `rhsBuffers`, `resultBuffers` | `laneN.aluClass.*` y `laneN.fpuClass.*` |
| `laneN.operandWaitCycles`, `resultWaitCycles`, `executionRetries` | `laneN.aluClass.*` y `laneN.fpuClass.*` |

Nuevas:

| Grupo | Métricas |
| --- | --- |
| `aluK` | `addElements`, `multiplyElements`, `divideElements`, `remainderElements`; `intervalStalls` incluye el divisor ocupado. `busyTicks` mide la ocupación de la instancia. |
| `fpu` | `add16Elements`, `add32Elements`, `multiply32Elements`, `divide32Elements`, `dividerStalls`; `capacityStalls` e `intervalStalls` se refieren al pipeline. |
| `aluClass`, `fpuClass` | `executionRetries` cuenta intentos rechazados por todas las unidades de la clase. |

Los `Retry` de cada instancia cuentan cada intento: al buscar la primera
instancia que acepta, las ocupadas registran su rechazo.

## Referencia normativa

RVV 1.0, manual oficial edición 20240411, [capítulo vectorial][rvv].
Secciones consultadas: *Vector Arithmetic Instruction encoding* (orden
`vs2 op vs1`), *Vector Single-Width Integer Multiply Instructions*,
*Vector Integer Divide Instructions* (casos extremos de la extensión M),
*Vector Floating-Point Instructions* (`frm`, `fflags`, `mstatus.FS`),
*Vector Single-Width Floating-Point Add/Subtract Instructions* y
*Vector Single-Width Floating-Point Multiply/Divide Instructions*. Latencias,
subrecursos y emisión por clase son decisiones del modelo.

## Comprobaciones y pendientes

Comprobaciones realizadas:

- `git diff --check`: correcto.
- `clang-format --dry-run --Werror` (20.1.8) sobre los C++ modificados:
  correcto.
- Black 25.11.0 e isort 7.0.0, en un entorno temporal, sobre los tres
  Python: correctos. `python3 -m py_compile`: correcto.
- SCons con objetivos bajo `build/RISCV/`: ALU, FPU, lane, soporte, fichas
  y `vector_engine`; enlace de `build/RISCV/gem5.opt`, que recompila también
  Minor y el frontend con las cabeceras modificadas. SCons avisa de que GCC
  15.2.1 está fuera de su rango.
- `configs/example/riscv_vpu.py --help` muestra las opciones nuevas. No se
  ha ejecutado ninguna carga.

Para el equipo se sugieren, sin implementar, y con comandos inyectados en
la VPU hasta que exista la parte de Minor: `vdiv`/`vrem` con divisor cero y
con mínimo entre −1 en los cuatro anchos; `vdiv.vv` con `vs1 ≠ vs2` para
fijar el orden de operandos; `vmul` con truncamiento; FP32 con NaN de
señalización, infinitos, ceros con signo, subnormales, los cinco modos de
redondeo y comprobación de flags; un `vfdiv` largo junto a una suma entera
independiente; y un `vdiv` con `alus_per_lane` 1 frente a 2, comparando
resultados, ciclos y `busyTicks` por instancia.

Quedan pendientes: la parte CPU de 2.6 ([pendientes de
Minor][pendientes-minor]) y, con ella, la evidencia desde un programa;
también la de 2.5 para observar ALU y FPU con instrucciones distintas
solapadas. Actualización del 11 de noviembre de 2026: las partes CPU de
2.5 y 2.6 están implementadas (ver el estado posterior de la frontera con
Minor); sigue pendiente la evidencia desde un programa. Quedan fuera: FP64, FP16 desde CPU, FMA, formas `.vf` y `.vi`,
conversiones, raíz cuadrada, `vmulh*`, widening/narrowing, máscaras,
latencia dependiente de datos, ancho SIMD distinto de la palabra, unidades
enteras dedicadas, límite de contextos por clase, y la red y las slides de
2.7.

[diseno]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md>
[cierre]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[pendientes-minor]: <Pendientes de Minor - Fases 1 a 5.md#fase-26-operaciones-enteras-y-fp32>
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
