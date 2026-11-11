# Hito 2: plan de implementación para cerrar la fase 8

Plan del 11 de noviembre de 2026 sobre `dad5ab6f5f` (rama `feature/vpu`).
Organiza lo que falta de la [sección 13.8 del diseño][diseno-f8] después
de la [entrega VPU de `vmin`/`vminu`][entrega-f8]. Es un plan de trabajo:
cada paquete se concretará en una propuesta de código o documentación y
necesitará aprobación antes de modificar archivos. No crea pruebas,
binarios ni resultados.

## Punto de partida

| Parte | Estado | Fuente |
| --- | --- | --- |
| VPU, fases 1--7 | Entregadas en backend; compiladas | Entregas de cada fase |
| VPU, `vmin`/`vminu` `.vv` | Implementado en ALU, 8/16/32/64 bits; compilado, sin ejecución | [Entrega VPU de fase 8][entrega-f8] |
| Minor, fases 2.1--2.7 | Implementado y verificado leyendo el código (P1); sin evidencia de ejecución | [Pendientes de Minor][pendientes-minor], [matriz][matriz] |
| Minor, decode de `vmin.vv` | Implementado (`963e6a9550`) | [Matriz][matriz] |
| Pathfinder | Ejecutado en offload y native con resultado contrastado (P2--P3) | [Plan de benchmarks][benchmarks], [entrega de fase 8][entrega-f8] |

Falta lo que la sección 13.8 exige además del código: ejecutar Pathfinder
(criterio 16 de la [sección 15.3][cierre]) y consolidar matriz,
observabilidad, criterios de cierre y entrega al hito 3.

## Decisiones tomadas

Acordadas con el usuario el 11 de noviembre de 2026:

| Decisión | Valor | Consecuencia |
| --- | --- | --- |
| Procedencia de Pathfinder | [RiVEC (RISC-V Vectorized Benchmark Suite)][rivec] | Revisión `32a14ddfb9acaeb677800e8d2e18a45d9421d4cc`; licencias BSD de Rodinia y BSC; adaptación sin libc descrita en el [README del benchmark](../../benchmarks/pathfinder/README.md). |
| Compilador | Clang | Las opciones acordadas, `-O2 -fno-vectorize -fno-slp-vectorize`, son de Clang y se usan literalmente. Localmente hay Clang 20.1.8; P2 registra la versión usada. |
| Validación de P3 | Verificación escalar dentro del programa y comparación de los modos offload (VPU) y nativo (RVV de gem5) | El equipo puede ampliar casos o tamaños; no es requisito previo de P3. |

## Condiciones detectadas

1. **libc.** El [baseline del hito 1](../../benchmarks/baseline/README.md)
   no enlaza libc (`-nostdlib`, `_start` propio) para que no aparezcan
   instrucciones RVV ajenas al kernel. Confirmado en P2: la glibc de
   `/opt/riscv` está compilada con V y el Pathfinder original enlazado con
   ella contiene unos 45 tipos RVV. La adaptación no usa libc.
2. **Adaptación de la fuente.** Si la revisión de RiVEC elegida usa
   intrinsics anteriores a RVV 1.0 o servicios de libc (E/S, tiempos,
   reserva dinámica), P2 propondrá la adaptación mínima y la registrará.
   La semántica del kernel no cambia.
3. **Perfil habilitado en el ejemplo.** `configs/example/riscv_vpu.py`
   usa los valores por defecto de `VpuConfiguration`: SEW 16/32 y sólo
   LMUL m1, sin opciones para cambiarlos. El kernel usa `e32, m1, ta, ma`,
   dentro de ese perfil, así que P3 no necesita cambiarlo.
4. **Dos ventanas de concurrencia.** Minor admite 2 instrucciones
   vectoriales en vuelo y la VPU 4 comandos activos por defecto. P5
   confirma que capacidad, contrapresión y drain son coherentes; con la
   ventana de Minor en 2, los 4 activos de la VPU no se alcanzan.

## Paquetes de trabajo

| # | Paquete | Trabajo | Entregable | Criterios 15.3 |
| --- | --- | --- | --- | --- |
| P1 | Reconciliar la documentación tras el merge | Actualizar las entregas de fases 5--7, el índice del hito y los pendientes de Minor, que aún describen a Minor sin `963e6a9550`. Distinguir lo declarado de lo que tiene evidencia. | Documentación coherente con el código actual. **Hecho el 11 de noviembre de 2026**; queda para P5 el límite de la ventana de Minor, que cuenta posiciones (incluidas entradas `Completed`) y no comandos activos | 9 |
| P2 | Pathfinder: fuente y compilación | `benchmarks/pathfinder/` con el patrón del baseline: fuente adaptada de RiVEC, Makefile con Clang, README. Kernel RVV explícito; copia inicial, inicialización y verificación escalares. Desensamblado y script que compruebe que sólo aparecen los 7 tipos RVV del kernel. | Binario reproducible con revisión, toolchain, opciones y desensamblado registrados **Hecho el 11 de noviembre de 2026** en [`benchmarks/pathfinder/`](../../benchmarks/pathfinder/README.md): RiVEC `32a14ddfb9`, Clang 20.1.8, sin libc; perfil de 7 tipos comprobado con `tiny` y `small` | 16 (toolchain y perfil) |
| P3 | Pathfinder: ejecución y contraste | Runner como `baseline/run.py`, modos offload y nativo, verificación escalar y comparación de salidas. Opciones de perfil SEW/LMUL en `riscv_vpu.py` si el kernel las necesita. | Ejecución funcional registrada y estadísticas de ALU, slide, red, VLSU y VRF **Hecho el 11 de noviembre de 2026** (`4c08b72fce`): `tiny` y `small` pasan en offload y native con huella idéntica; evidencia en la [entrega de fase 8](<Fase 8 - Mínimo entero para Pathfinder.md#evidencia-de-ejecución-de-pathfinder>) | 16; en parte 6, 7 y 11 |
| P4 | Observabilidad (punto 4 de 13.8) | Revisar que las estadísticas explican dependencias, unidades, VRF, memoria y red con unidades claras y sin doble recuento, incluidas las esperas nuevas de Minor frente a las de la VPU. Usar Pathfinder como carga real. | Informe y correcciones mínimas, si hacen falta **Hecho el 11 de noviembre de 2026**: [análisis de observabilidad](<Observabilidad - análisis con Pathfinder.md>), con reparto del tiempo, criterio 5 para bancos, lanes y red, 7 hallazgos y 5 correcciones propuestas. Pathfinder no sirve para el criterio 1: su kernel es serie | 7, 12 |
| P5 | Repaso de extremo a extremo (punto 6) | Propiedad de buffers, identidad (`CommandKey`, `TaskKey`, `VrfAccessKey`, `requestId`), reservas, liberación y drain con las dos ventanas. Comprobar que `VectorSupport` es la única autoridad de soporte. | Informe; los cambios, en propuestas aparte **Hecho el 11 de noviembre de 2026**: [revisión](<Revisión de extremo a extremo - Minor y VPU.md>) sin defectos de corrección y con cinco matices de semántica | 2, 8, 15 |
| P6 | Matriz y fichas (puntos 1--2) | Una matriz por operación, forma, ancho y ruta que separe implementado, comprobado, parcial y candidato, unificando la matriz de decode y la tabla de la fase 6. Plantilla de ficha para la siguiente instrucción. | Matriz consolidada y plantilla **Hecho el 11 de noviembre de 2026**: [matriz vigente](<Matriz de decode, representacion y soporte.md>) con niveles de evidencia y huecos, y [plantilla de ficha](<Plantilla de ficha de operación.md>) con `vmin` como ejemplo; baseline del hito 1 ejecutado de nuevo con `PASS` | 9 |
| P7 | Cierre frente a 15.3 (punto 5) | Estado de los 16 criterios: evidencia registrada, implementado sin evidencia o pendiente, con enlaces a las entregas. | Documento de cierre del hito 2 **Hecho el 11 de noviembre de 2026**: [cierre del hito 2](<Cierre del hito 2 - Estado de los criterios.md>), cerrado con pendientes de evidencia (con P8: 7 criterios acreditados, 8 parciales, 1 implementado sin evidencia) | Todos |
| P8 | Entrega al hito 3 (puntos 3 y 7) | Contratos que deben ampliarse (disponibilidad por rangos, predicación, resultados escalares, colectivos, memoria concurrente, clústeres) y campaña de micro-kernels propuesta con su ROI, sin implementarla. | Sección de traspaso y ficha de campaña **Hecho el 11 de noviembre de 2026**: [entrega al hito 3](<Entrega al hito 3 - Contratos y campaña.md>), con mapa de contratos, garantías afectadas, datos de partida, campaña de 7 micro-kernels y decisiones abiertas. **Fase 8 terminada** | 15 |

Trabajo adicional aprobado el 11 de noviembre de 2026, derivado de P4:

- **Correcciones de métricas** (`2ef0927059`): esperas de Minor por
  intervalos, `hazardWaitTicks`, descripciones y ceros visibles con
  offload. Ver las correcciones aplicadas del [análisis de
  observabilidad](<Observabilidad - análisis con Pathfinder.md>).
- **Micro-kernel de concurrencia** (`67c03af94b`,
  [`benchmarks/concurrency/`](../../benchmarks/concurrency/README.md)):
  evidencia del criterio 1 y del efecto de ALUs y puertos (criterio 5),
  con un runner común para los benchmarks.

Orden: P1 → P2 → P3 → (P4 y P5 en paralelo) → P6 → P7 → P8. P1 es barato
y deja una base fiable; P4 y P5 aprovechan las trazas de P3; P7 depende
de los anteriores. Cada paquete termina en su propio commit.

## Fuera de alcance

- Decode de `vminu.vv` en Minor, salvo decisión expresa; corresponde a la
  rama de Minor.
- Formas `.vx`, `vmax`/`vmaxu` y formas con máscara.
- Calibración temporal frente a Ara.
- AXPY y MatMul, que cierran los hitos 3 y 4.
- Pruebas de los criterios 1, 3, 4, 6, 10, 11 y 13: las define el equipo.
  P7 sólo registra la evidencia que exista.

[diseno-f8]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#138-fase-8-extensión-observabilidad-y-cierre>
[cierre]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#153-criterios-de-cierre-del-hito>
[entrega-f8]: <Fase 8 - Mínimo entero para Pathfinder.md>
[pendientes-minor]: <Pendientes de Minor - Fases 1 a 5.md>
[matriz]: <Matriz de decode, representacion y soporte.md>
[benchmarks]: <../Plan de benchmarks.md>
[rivec]: https://github.com/RALC88/riscv-vectorized-benchmark-suite
