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
| Minor, fases 2.1--2.7 | Declarado resuelto por su autor; evidencia de ejecución por confirmar | [Pendientes de Minor][pendientes-minor], [matriz][matriz] |
| Minor, decode de `vmin.vv` | Implementado (`963e6a9550`) | [Matriz][matriz] |
| Pathfinder | Sin fuentes en el repositorio, sin binario ni ejecución | [Plan de benchmarks][benchmarks] |

Falta lo que la sección 13.8 exige además del código: ejecutar Pathfinder
(criterio 16 de la [sección 15.3][cierre]) y consolidar matriz,
observabilidad, criterios de cierre y entrega al hito 3.

## Decisiones tomadas

Acordadas con el usuario el 11 de noviembre de 2026:

| Decisión | Valor | Consecuencia |
| --- | --- | --- |
| Procedencia de Pathfinder | [RiVEC (RISC-V Vectorized Benchmark Suite)][rivec] | P2 fija la revisión exacta, la licencia y la adaptación necesaria. |
| Compilador | Clang | Las opciones acordadas, `-O2 -fno-vectorize -fno-slp-vectorize`, son de Clang y se usan literalmente. Localmente hay Clang 20.1.8; P2 registra la versión usada. |
| Validación de P3 | Verificación escalar dentro del programa y comparación de los modos offload (VPU) y nativo (RVV de gem5) | El equipo puede ampliar casos o tamaños; no es requisito previo de P3. |

## Condiciones detectadas

1. **libc.** El [baseline del hito 1](../../benchmarks/baseline/README.md)
   no enlaza libc (`-nostdlib`, `_start` propio) para que no aparezcan
   instrucciones RVV ajenas al kernel. Una libc compilada con V podría
   introducir otros tipos RVV y romper el perfil de 7. Pathfinder seguirá
   el mismo patrón salvo que el desensamblado demuestre que no hace falta.
2. **Adaptación de la fuente.** Si la revisión de RiVEC elegida usa
   intrinsics anteriores a RVV 1.0 o servicios de libc (E/S, tiempos,
   reserva dinámica), P2 propondrá la adaptación mínima y la registrará.
   La semántica del kernel no cambia.
3. **Perfil habilitado en el ejemplo.** `configs/example/riscv_vpu.py`
   usa los valores por defecto de `VpuConfiguration`: SEW 16/32 y sólo
   LMUL m1, sin opciones para cambiarlos. Si el kernel usa otro LMUL, P3
   añadirá esas opciones.
4. **Dos ventanas de concurrencia.** Minor admite 2 instrucciones
   vectoriales en vuelo y la VPU 4 comandos activos por defecto. No es un
   error, pero P5 debe revisar que capacidad, contrapresión y drain son
   coherentes de extremo a extremo.

## Paquetes de trabajo

| # | Paquete | Trabajo | Entregable | Criterios 15.3 |
| --- | --- | --- | --- | --- |
| P1 | Reconciliar la documentación tras el merge | Actualizar las entregas de fases 5--7, el índice del hito y los pendientes de Minor, que aún describen a Minor sin `963e6a9550`. Distinguir lo declarado de lo que tiene evidencia. | Documentación coherente con el código actual | 9 |
| P2 | Pathfinder: fuente y compilación | `benchmarks/pathfinder/` con el patrón del baseline: fuente adaptada de RiVEC, Makefile con Clang, README. Kernel RVV explícito; copia inicial, inicialización y verificación escalares. Desensamblado y script que compruebe que sólo aparecen los 7 tipos RVV del kernel. | Binario reproducible con revisión, toolchain, opciones y desensamblado registrados | 16 (toolchain y perfil) |
| P3 | Pathfinder: ejecución y contraste | Runner como `baseline/run.py`, modos offload y nativo, verificación escalar y comparación de salidas. Opciones de perfil SEW/LMUL en `riscv_vpu.py` si el kernel las necesita. | Ejecución funcional registrada y estadísticas de ALU, slide, red, VLSU y VRF | 16; en parte 6, 7 y 11 |
| P4 | Observabilidad (punto 4 de 13.8) | Revisar que las estadísticas explican dependencias, unidades, VRF, memoria y red con unidades claras y sin doble recuento, incluidas las esperas nuevas de Minor frente a las de la VPU. Usar Pathfinder como carga real. | Informe y correcciones mínimas, si hacen falta | 7, 12 |
| P5 | Repaso de extremo a extremo (punto 6) | Propiedad de buffers, identidad (`CommandKey`, `TaskKey`, `VrfAccessKey`, `requestId`), reservas, liberación y drain con las dos ventanas. Comprobar que `VectorSupport` es la única autoridad de soporte. | Informe; los cambios, en propuestas aparte | 2, 8, 15 |
| P6 | Matriz y fichas (puntos 1--2) | Una matriz por operación, forma, ancho y ruta que separe implementado, comprobado, parcial y candidato, unificando la matriz de decode y la tabla de la fase 6. Plantilla de ficha para la siguiente instrucción. | Matriz consolidada y plantilla | 9 |
| P7 | Cierre frente a 15.3 (punto 5) | Estado de los 16 criterios: evidencia registrada, implementado sin evidencia o pendiente, con enlaces a las entregas. | Documento de cierre del hito 2 | Todos |
| P8 | Entrega al hito 3 (puntos 3 y 7) | Contratos que deben ampliarse (disponibilidad por rangos, predicación, resultados escalares, colectivos, memoria concurrente, clústeres) y campaña de micro-kernels propuesta con su ROI, sin implementarla. | Sección de traspaso y ficha de campaña | 15 |

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
