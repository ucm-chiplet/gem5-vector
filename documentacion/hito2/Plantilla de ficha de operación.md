# Plantilla de ficha de operación

Ficha para proponer una instrucción vectorial nueva o ampliar una existente
(paquete P6 del [plan de cierre de la fase 8][plan-f8]). Sigue el
procedimiento de la sección 11.2 del [diseño del hito 2][diseno-11] y las
reglas de `AGENTS.md`: la semántica la fija el manual de RVV 1.0, no el
código de gem5 ni de Ara, y ningún archivo de la VPU se modifica sin una
propuesta aprobada.

Copia la plantilla en la propuesta, rellena cada apartado y deja escrito
"no aplica" en vez de borrarlo. Al final hay un ejemplo con `vmin`/`vminu`.

## Procedimiento

1. Identificar la carga que la necesita, la forma y el ancho.
2. Consultar su sección del [manual RVV 1.0, edición 20240411][rvv] y
   rellenar la ficha.
3. Presentar la propuesta (ficha, archivos, validación sugerida) y esperar
   aprobación.
4. Añadir la traducción en Minor (`decode.cc`) si falta.
5. Registrar la ficha en `OperationTraits` y añadir el cálculo y su perfil
   temporal en la unidad.
6. Reutilizar tareas, dependencias, transporte y cierres de su familia;
   una familia nueva puede necesitar contratos nuevos.
7. Actualizar la [matriz vigente][matriz] y documentar las comprobaciones.

## Plantilla

### 1. Motivo y alcance

- Carga o benchmark que la necesita y objetivo (O1--O6) al que contribuye.
- Formas (`.vv`, `.vx`, `.vi`, `.vf`...) y anchos que se proponen.
- Qué queda fuera de esta propuesta.

### 2. Referencia normativa

- Edición, sección y enlace del manual.
- Codificación (`funct6`, categoría OPIVV/OPIVX/OPMVV/OPFVV...).

### 3. Semántica

| Aspecto | Contenido |
| --- | --- |
| Resultado por elemento | Fórmula exacta, con `vs2` como primer operando |
| Signo y formato | Entero con o sin signo, FP y formato |
| Anchos | SEW admitidos; EEW/EMUL de cada operando si difieren |
| Grupos y solapamientos | Restricciones de registros, codificaciones reservadas |
| Máscara (`vm=0`) | Admitida o rechazada antes de efectos |
| `vl`, `vstart`, tail | Rango activo, reanudación, política de tail del hito |
| Redondeo y flags | Modo usado, flags que puede producir |
| Excepciones | Faults posibles y cuándo se notifican |
| Casos extremos | División por cero, desbordamiento, NaN, etc. |

### 4. Microarquitectura

| Aspecto | Contenido |
| --- | --- |
| Clase de ejecución | ALU, FPU, VLSU, SlideUnit u otra aprobada |
| Ficha `OperationTraits` | Formato, anchos backend, anchos admitidos desde CPU, formas, signo, contexto FP, faults, solapamiento destino--fuente |
| Perfil temporal | Latencia e intervalo; segmentada o no; parámetros nuevos y valores por defecto |
| Recursos | Buffers, lecturas y escrituras del VRF, red, memoria |
| Estadísticas | Contadores nuevos o reutilizados |

### 5. Integración CPU--VPU

- Traducción en Minor: mnemónicos, campos, orden de operandos y escalar.
- Captura de configuración, contexto FP y `mstatus` (`FS`, `VS`).
- Si entra en la ventana de concurrencia de Minor.
- Efectos al retirar: `fflags`, `vstart`, faults.

### 6. Archivos y módulos

Lista de archivos que se crean o modifican y responsabilidad de cada uno.

### 7. Validación sugerida

Programa o micro-kernel, casos (incluidos los extremos y los rechazos),
configuración y métrica que demostraría que funciona. Se propone; no se
implementa sin aprobación.

### 8. Estado y evidencia

Nivel en la matriz (implementado, ejecutado, candidato), commits y
registros de ejecución.

## Ejemplo: `vmin` y `vminu`

Rellenado a partir de la [entrega de la fase 8][fase8].

**1. Motivo y alcance.** Pathfinder (cierre del hito 2) necesita
`vmin.vv` con signo de 32 bits; contribuye a O2 y O6. Se aprobó ampliar a
SEW 8/16/32/64 y a `vminu`. Fuera: formas `.vx`, `vmax`/`vmaxu`, máscara.

**2. Referencia.** RVV 1.0, edición 20240411, *Vector Integer Min/Max
Instructions*. OPIVV, `funct6` 000100 (`vminu`) y 000101 (`vmin`).

**3. Semántica.**

| Aspecto | Contenido |
| --- | --- |
| Resultado por elemento | `vd[i] = min(vs2[i], vs1[i])` |
| Signo y formato | `vmin`: complemento a dos; `vminu`: natural |
| Anchos | SEW 8/16/32/64; todos los operandos con EEW = SEW y EMUL = LMUL |
| Grupos y solapamientos | Grupos alineados; `vd` puede coincidir con una fuente |
| Máscara | Rechazada antes de efectos |
| `vl`, `vstart`, tail | Comunes a las operaciones enteras de la fase 6 |
| Redondeo y flags | No aplica: el resultado es uno de los operandos |
| Excepciones | Ninguna tras la admisión |
| Casos extremos | Mínimo y máximo representables; iguales |

**4. Microarquitectura.** ALU. Ficha `integerTraits(true, false)`:
formato entero, SEW 8/16/32/64 en backend y desde CPU, sólo `.vv`, signo
obligatorio, sin contexto FP ni faults, destino puede solapar fuente.
Perfil segmentado propio, `alu_minimum_latency` y
`alu_minimum_initiation_interval` (1/1 por defecto). Recursos de la clase
ALU. Estadística `minimumElements`.

**5. Integración.** Minor traduce `vmin_vv` a `Minimum` con `Signed`;
`vminu.vv` no se traduce. Entra en la ventana aritmética. Sin efectos FP.

**6. Archivos.** `operation_traits.cc`, `alu.hh/.cc`, `vpu_parameters.hh`,
`vector_engine.cc`, `VectorEngine.py`, `configuration.py`; decode en
`src/cpu/minor/decode.cc`.

**7. Validación.** Microprograma `vle`/`vmin`/`vse` en los cuatro SEW
con valores extremos, LMUL m1 y m2, `vl < VLMAX` y rechazo de `vm=0`.

**8. Estado.** Implementado (`2c3b45792f`). Ejecutado `vmin.vv` a SEW 32,
m1, en Pathfinder y en el micro-kernel de concurrencia; resto de anchos y
`vminu` sin evidencia ([matriz][matriz]).

[plan-f8]: <Plan de cierre de la fase 8.md>
[diseno-11]: <Hito 2 - Concurrencia, FPU, anchos e interconexión.md#112-procedimiento-para-añadir-una-operación>
[matriz]: <Matriz de decode, representacion y soporte.md>
[fase8]: <Fase 8 - Mínimo entero para Pathfinder.md>
[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
