# Fase 8 del hito 2: mínimo entero para Pathfinder, entrega VPU

Implementación autorizada por el usuario el 10 de noviembre de 2026, sobre
`963e6a9550` (rama `feature/vpu` tras integrar por fast-forward
`origin/feature/vpu-minorcpu`), con el alcance de la propuesta presentada
en la conversación y sus decisiones:

- la operación es `ArithmeticOperation::Minimum`, que la rama de Minor ya
  genera al decodificar `vmin.vv`;
- se ejecuta en la ALU de cada lane, sólo en forma `.vv`;
- se admiten los dos signos: `Signed` (`vmin`) y `Unsigned` (`vminu`);
- backend y admisión desde CPU cubren SEW 8/16/32/64;
- tiene un perfil temporal propio, `alu_minimum_*`, con el mismo valor por
  defecto que la suma.

**Ampliación respecto al diseño.** La [sección 13.8][diseno-f8] limitaba la
ampliación a `vmin.vv` con signo de 32 bits ("no incorpora otros anchos de
mínimo ni familias completas"). El usuario aprobó abrir el mínimo a todos
los anchos enteros y a ambos signos, porque la ALU ya calcula en 8/16/32/64
bits y la diferencia es sólo la comparación. Pathfinder sigue necesitando
únicamente `vmin.vv` con SEW=32; el resto es capacidad adicional. El texto
de la sección 13.8 se ha ajustado en consecuencia.

La entrega atiende O2 y O6:

- **O2:** completa en la VPU los 7 tipos RVV de Pathfinder, la primera
  carga real prevista para observar los recursos finitos del hito 2.
- **O6:** la combinación pasa de "decodificada en Minor, pendiente de
  backend" a operación con ficha, unidad, temporización y estadística
  propias.

## Alcance y estado de la evidencia

La VPU sabe admitir, planificar, temporizar y cerrar:

- `vmin` y `vminu` en forma `.vv`, sin predicación, con SEW 8/16/32/64 y
  todos los LMUL del perfil habilitado. `vl`, `vstart`, rangos vacíos y
  tail siguen el mismo camino que las demás operaciones enteras de la
  [fase 6][fase6].

**Frontera con Minor.** Minor decodifica `vmin.vv` (con signo) desde
`963e6a9550` y no restringe su SEW, que toma de `vtype`. `vminu.vv` no
se decodifica: su soporte es de backend y admisión, inalcanzable desde un
programa hasta que Minor lo traduzca. Las formas `.vx` y `vmax`/`vmaxu`
no están representadas en la ficha.

Compilar esta entrega no acredita ejecución funcional ni validación
temporal. No se han ejecutado cargas ni se han creado pruebas. Pathfinder
no se ha compilado ni ejecutado; el criterio 16 de la
[sección 15.3][cierre] sigue pendiente.

**Actualización del 11 de noviembre de 2026.** Pathfinder ya se ha
compilado y ejecutado con `vmin.vv` en la VPU: ver la
[evidencia de ejecución](#evidencia-de-ejecución-de-pathfinder).

## Archivos

Bajo `src/cpu/vector_engine/`:

| Archivos | Cambio |
| --- | --- |
| `common/operation_traits.cc` | Ficha `minimum`: `integerTraits(true, false)`. `integerTraits` recibe ahora si admite `.vx` (por defecto sí). |
| `vpu/lanes/alu.hh/.cc` | `integerMinimum()`, casos en `computeInteger`, `timing` y `countElements`; validación del perfil en el constructor; contador `minimumElements`. |
| `common/vpu_parameters.hh` | `AluParameters::minimum`, incluido en `currentProfile()`. |
| `vector_engine.cc` | Conecta `alu_minimum_latency` y `alu_minimum_initiation_interval`. |
| `VectorEngine.py`, `configuration.py` | Parámetros `alu_minimum_*` y campos `AluConfiguration.minimum_*`. |
| `common/vector_types.hh`, `common/execution_types.hh` | Sólo comentarios: papel de `Minimum` y de su signo. |

`VectorSupport` no cambia: la ficha basta para que `checkCommand` aplique
anchos, signo, forma `.vv` y `vm=1`. La lane envía el trabajo a la ALU por
`ExecutionClass::IntegerAlu`, sin cambios en `ara_lane.cc`.

## Semántica

Para cada elemento activo, `vd[i] = min(vs2[i], vs1[i])`:

- `Signed`: los SEW bits se interpretan en complemento a dos con el
  `signExtend()` existente, que evita conversiones dependientes de la
  implementación con SEW=64.
- `Unsigned`: se comparan los SEW bits como naturales.

El resultado es uno de los operandos sin modificar, de modo que no hay
overflow, redondeo ni flags. La operación es conmutativa; se conserva la
convención `lhs = vs2`, `rhs = vs1` de las demás operaciones.

Ficha:

| Campo | Valor |
| --- | --- |
| Unidad | `IntegerAlu` |
| Formato | `Integer` |
| Anchos backend / CPU | 8/16/32/64 / 8/16/32/64 |
| Formas | `.vv` |
| Signo | Obligatorio (`Signed` o `Unsigned`) |
| Contexto FP | No |
| Fault de memoria | No |
| `vd` puede solapar una fuente | Sí (igual ancho) |

Un comando `Minimum` con `NotApplicable` se rechaza como `InvalidPayload`;
uno con escalar (`.vx`) o con un ancho no admitido, como
`UnsupportedOperation`.

## Parámetros

| Parámetro (`VectorEngine`) | `AluConfiguration` | Defecto | Significado |
| --- | --- | --- | --- |
| `alu_minimum_latency` | `minimum_latency` | 1 | Ciclos VPU hasta el resultado. |
| `alu_minimum_initiation_interval` | `minimum_initiation_interval` | 1 | Ciclos VPU entre admisiones de mínimo en una instancia. |

El comparador se modela segmentado, como la suma. Los valores son
parámetros del modelo, no latencias calibradas de Ara. No se añade opción
en `configs/example/riscv_vpu.py`, igual que para la suma.

## Estadísticas

| Grupo | Métrica |
| --- | --- |
| `lane*.alu*` | `minimumElements`: elementos admitidos de mínimos enteros. También cuentan en `elements8/16/32/64`. |

## Evidencia de ejecución de Pathfinder

Registrada el 11 de noviembre de 2026 en los paquetes P2 y P3 del
[plan de cierre][plan-f8]:

| Dato | Valor |
| --- | --- |
| Código | `4c08b72fce` (`feature/vpu`); `build/RISCV/gem5.opt` compilado desde `dad5ab6f5f`, sin cambios posteriores en `src/` |
| Benchmark | [`benchmarks/pathfinder/`](../../benchmarks/pathfinder/README.md), RiVEC `32a14ddfb9`, adaptado sin libc |
| Toolchain | Clang 20.1.8, `llvm-objdump` 20.1.8, GNU ld 2.46 |
| Opciones | `-O2 -fno-vectorize -fno-slp-vectorize -march=rv64gcv`, sin libc; perfil RVV comprobado: sólo los 7 tipos del kernel, todos en `runVector` |
| Configuración | `configs/example/riscv_vpu.py` por defecto: VLEN 256, 2 lanes, crossbar, `e32m1` |
| Comando | `python3 benchmarks/pathfinder/run.py --input {tiny,small}` |

Los dos modos, `offload` (VPU) y `native` (RVV de MinorCPU), terminan con
código 0, coinciden con la referencia de la entrada y con la versión
escalar del programa, y dan la misma huella:

| Entrada | Huella | Ticks del kernel, offload | Ticks del kernel, native |
| --- | --- | --- | --- |
| `tiny` (32×32) | `sum=1731 xor=29` | 52 083 000 | 36 419 000 |
| `small` (128×1024) | `sum=188600 xor=46` | 5 995 385 000 | 3 545 065 000 |

Región del kernel en offload (entre `m5_reset_stats` y `m5_dump_stats`):

| Métrica | `tiny` | `small` |
| --- | --- | --- |
| Comandos despachados por Minor y completados por la VPU | 992 | 130 048 |
| Elementos de mínimo / suma en ALU | 1 984 / 992 | 260 096 / 130 048 |
| Tareas `vslide1up` / `vslide1down` | 124 / 124 | 16 256 / 16 256 |
| Paquetes de red remotos / locales | 744 / 0 | 97 536 / 0 |
| Comandos de load / store | 248 / 124 | 32 512 / 16 256 |
| Máximo de comandos activos en la VPU | 2 | 2 |
| Ciclos con más de un comando activo (Minor) | 1 736 | 227 584 |
| Máximo de peticiones de memoria en curso | 1 | 1 |

Los recuentos coinciden con el kernel: cada tira de 8 elementos genera
2 loads, 2 slides, 2 mínimos, 1 suma y 1 store (124 tiras en `tiny` y
16 256 en `small`); `vsetvli` se ejecuta en CPU.

Qué acredita y qué no:

- **Criterio 16, parte funcional:** Pathfinder ejecuta sus 7 tipos RVV con
  copia inicial escalar, resultado contrastado y recursos finitos, y se
  registran toolchain, opciones y desensamblado. Falta el análisis de
  tráfico y comportamiento temporal (paquete P4).
- **Observaciones parciales de otros criterios,** con SEW=32, m1 y la
  configuración por defecto: dos instrucciones del programa activas a la
  vez en la VPU (criterio 1), slides desde programa cruzando lanes por la
  red (criterio 6) y un único paquete de memoria en vuelo (criterio 11).
  No cubren otros anchos, `vstart` distinto de 0 ni otras
  configuraciones.
- Los ticks son del modelo, no una medida calibrada de Ara. La diferencia
  entre offload y native no es todavía una conclusión: su análisis
  corresponde a P4.

## Referencia normativa

RVV 1.0, manual oficial edición 20240411, [capítulo vectorial][rvv],
sección *Vector Integer Min/Max Instructions*: `vminu` y `vmin` calculan el
mínimo sin y con signo elemento a elemento, en formas `.vv` y `.vx`. Las
reglas de `vl`, `vstart`, máscara y tail son las generales del capítulo.
La temporización es una decisión del modelo.

## Comprobaciones y pendientes

Comprobaciones realizadas:

- `git diff --check`: correcto.
- `clang-format --dry-run --Werror` (20.1.8) sobre los C++ modificados:
  correcto tras aplicar el formato a las líneas nuevas.
- Black 26.10.1 e isort, en un entorno temporal, sobre los dos Python:
  correctos. `python3 -m py_compile`: correcto.
- SCons: compilación y enlace de `build/RISCV/gem5.opt`, que recompila
  también Minor. SCons avisa de que GCC 15.2.1 no está oficialmente
  soportado; no hay otros avisos ni errores.

Pendientes fuera de esta entrega, organizados en el
[plan de cierre de la fase 8](<Plan de cierre de la fase 8.md>):

- Análisis de tráfico y comportamiento temporal de Pathfinder (P4); la
  ejecución funcional está registrada arriba.
- Decode de `vminu.vv` en Minor, si se quiere alcanzar desde programa.
- Formas `.vx`, `vmax`/`vmaxu` y formas con máscara.
- Resto de la consolidación de la fase 8 (sección 13.8, puntos 1--7).
- Actualizar las entregas de fases 5--7, que aún describen a Minor sin la
  integración de `963e6a9550`.

Prueba sugerida al equipo (no implementada): microprograma
`vsetvli` → `vle` ×2 → `vmin.vv` → `vse` en SEW 8/16/32/64, con el mínimo
y el máximo representables y negativos frente a positivos; LMUL m1 y m2;
`vl < VLMAX` para el tail; y rechazo de `vm=0`. Comprobar
`minimumElements` en las estadísticas.

[diseno-f8]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#138-fase-8-extensión-observabilidad-y-cierre>
[cierre]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[fase6]: <Fase 6 - FPU, operaciones enteras y varias ALUs.md>
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
