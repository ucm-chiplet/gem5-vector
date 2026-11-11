# Matriz de decode, representación y soporte (hito 2)

**Matriz vigente** del hito 2, actualizada el 11 de noviembre de 2026
sobre `c02a2da837` (paquete P6 del [plan de cierre de la fase 8][plan-f8]).
Sustituye a las tablas de las entregas de fase, que describen el estado en
su fecha. Separa, como pide la Fase 2.1 (punto 3) y la sección 2.2
(acción 6) del diseño:

1. **Decode en MinorCPU** (`src/cpu/minor/decode.cc`): qué instrucciones
   RISC-V traduce Minor a un comando para la VPU.
2. **Representación** (`src/cpu/vector_engine/interface/`): qué puede
   transportar un `VectorCommand`.
3. **Admisión y backend** (`OperationTraits` y `VectorSupport`): qué
   combinaciones de operación, ancho y grupo acepta y ejecuta la VPU.
4. **Evidencia**: qué se ha ejecutado desde un programa, con qué carga y
   configuración, y con qué resultado.

También distingue una ilegalidad según RVV 1.0 de un rechazo por
limitación del modelo.

## 1. Matriz por operación

**Niveles.** *Implementado*: decode, admisión y backend existen y
compilan. *Ejecutado*: un programa lo ejecuta en offload con resultado
verificado y la misma huella que en RVV nativo. Todo lo demás es
*candidato* o está fuera del hito.

**Perfil habilitado.** El backend admite SEW 8/16/32/64 y los LMUL del
perfil de la fase 2 (m1--m8 y mf2--mf8 según SEW y ELEN). El ejemplo
`configs/example/riscv_vpu.py` sólo habilita **SEW 16/32 y LMUL m1** por
defecto; otros valores se habilitan con
`--vpu-set supported_sews=8,16,32,64` y `--vpu-set supported_lmuls=...`
(exponentes de dos, -3 para mf8 y 3 para m8).

**Reglas comunes.** Ninguna operación admite máscara: `vm=0` se rechaza
antes de producir efectos. `vsetvli` se ejecuta en CPU. Las formas `.vv`
usan `vd = vs2 op vs1` y las `.vx`, `vd = vs2 op rs1` con el escalar
normalizado por CPU. Un rechazo de admisión llega al programa como
`IllegalInstFault` al retirar.

| Operación y forma | Decode Minor | Admisión VPU (SEW) | Unidad | Ejecutado desde programa | Falta |
| --- | --- | --- | --- | --- | --- |
| `vadd.vv` | Sí | 8/16/32/64 | ALU | SEW 32, m1: Pathfinder, concurrencia, baseline del hito 1 | Otros SEW y LMUL |
| `vadd.vx` | Sí | 8/16/32/64 | ALU | SEW 32, m1: baseline del hito 1 (21 casos) | Otros SEW y LMUL |
| `vmul.vv` | Sí | 8/16/32/64 | ALU | SEW 32, m1: concurrencia `integer` | Otros SEW y LMUL |
| `vmul.vx` | Sí | 8/16/32/64 | ALU | No | Todo |
| `vdiv.vv/.vx`, `vdivu.vv/.vx` | Sí | 8/16/32/64; signo obligatorio | ALU (divisor no segmentado) | No | Todo, incluidos divisor cero y desbordamiento |
| `vrem.vv/.vx`, `vremu.vv/.vx` | Sí | 8/16/32/64; signo obligatorio | ALU (divisor no segmentado) | No | Todo, incluidos divisor cero y desbordamiento |
| `vmin.vv` | Sí (con signo) | 8/16/32/64 | ALU | SEW 32, m1: Pathfinder, concurrencia | Otros SEW y LMUL |
| `vminu.vv` | No | 8/16/32/64 | ALU | No (inalcanzable desde programa) | Decode en Minor |
| `vfadd.vv` | Sí | 32 (FP32); binary16 sólo interno | FPU | FP32, m1: concurrencia `mixed`, sin flags IEEE | Flags, modos de redondeo, valores especiales |
| `vfmul.vv` | Sí | 32 (FP32) | FPU | FP32, m1: concurrencia `mixed`, sin flags IEEE | Flags, modos de redondeo, valores especiales |
| `vfdiv.vv` | Sí | 32 (FP32) | FPU (divisor no segmentado) | No | Todo |
| `vle32.v`, `vse32.v` | Sí | EEW = SEW = 32 | VLSU | m1: Pathfinder, concurrencia, baseline | Faults, `vstart` ≠ 0 |
| `vle8/16/64.v`, `vse8/16/64.v` | Sí | EEW = SEW | VLSU | No | Todo |
| `vslide1up.vx`, `vslide1down.vx` | Sí | 8/16/32/64; `vd = vs2` reservado en up | SlideUnit y red | SEW 32, m1: Pathfinder (cruzando lanes) | Otros SEW y LMUL, `vstart` ≠ 0 |
| Memoria strided e indexada | No | No (`supportsMemory` exige unit-stride) | — | — | Hitos 4 y 5 |
| Otras operaciones (`vsub`, lógicas, `.vi`, reducciones, máscaras...) | No | No (no existen en `ArithmeticOperation`) | — | — | Según el plan de evolución |

Cargas usadas como evidencia, todas en la configuración por defecto,
código `67c03af94b` (sólo cambia documentación hasta `c02a2da837`):

| Carga | Ejecución | Registro |
| --- | --- | --- |
| [Pathfinder][pathfinder] `tiny` y `small` | Offload y native, huella idéntica, verificación completa | [Entrega de la fase 8][fase8] |
| [Concurrencia][concurrency] `integer` y `mixed`, 1 024 elementos | Offload y native, huella idéntica, verificación escalar completa | [Análisis de observabilidad][observabilidad] |
| [Baseline del hito 1][baseline]: `baseline` (21 casos) y `baseline-simple` | Offload y native, `PASS`, ejecutado de nuevo el 11 de noviembre de 2026 | Resultados locales de `benchmarks/baseline/` |

## 2. Distinción normativa RVV 1.0 vs. Limitación del Modelo

### Regla en el estándar RISC-V Vector 1.0
En el manual oficial de RVV 1.0 (edición 20240411, sección *Vector Load/Store
Width Encoding*):
* Las instrucciones de carga y almacenamiento unit-stride codifican
  explícitamente el ancho efectivo del elemento en memoria (**EEW**):
  8, 16, 32 o 64 bits.
* La configuración activa en los CSRs vectoriales define el ancho estándar
  del elemento aritmético (**SEW**) y el multiplicador (**LMUL**).
* **La ISA permite legalmente combinaciones donde `EEW != SEW`**. En tales
  casos, el multiplicador efectivo para la transferencia de memoria se calcula
  dinámicamente como:
  $$\text{EMUL} = \frac{\text{EEW}}{\text{SEW}} \times \text{LMUL}$$
  Siempre que $1/8 \le \text{EMUL} \le 8$, la instrucción es completamente
  válida y debe ejecutarse transfiriendo $\text{vl}$ elementos de ancho $\text{EEW}$
  hacia o desde los registros correspondientes.

### Restricción arquitectónica en el modelo gem5 VPU
En el modelo actual de la VPU Ara-like:
* Para simplificar la microarquitectura de la VLSU y evitar gestionar
  dinámicamente agrupamientos fraccionales o multiplicados distintos a LMUL
  en las transferencias entre lanes y memoria, `VectorSupport::supportsMemory`
  establece como invariante:
  ```cpp
  memory.elementWidthBits == sew_bits
  ```
* Si un programa RISC-V emite, por ejemplo, `vle32.v` cuando el estado activo
  es `SEW=16`, la instrucción se decodifica en MinorCPU con `EEW=32`, pero al
  solicitar la reserva (`requestGrant`), la VPU la rechaza con el motivo:
  `RejectionReason::UnsupportedConfiguration`.
* MinorCPU captura este motivo en `MinorVectorState` y, en la fase de retiro
  (`commitInst`), produce un `RiscvISA::IllegalInstFault`.

> [!IMPORTANT]
> El `IllegalInstFault` resultante de una ejecución con `EEW != SEW` no denota
> que el binario contenga una instrucción fuera de la especificación RISC-V,
> sino que **el entorno de simulación / perfil de VPU activo no implementa
> el soporte para transferencias vectoriales con anchos dispares (`EEW != SEW`)**.

---

## 3. Resumen de capas y responsabilidades

```
+-------------------------------------------------------------------+
| 1. MinorCPU Decode (decode.cc)                                    |
|    - Filtra instrucciones RVV soportadas para offload            |
|    - Extrae campos inmediatos, EEW de machInst, registros fuente  |
|    - Reconoce macro-ops de slide sin descomposición a micro-ops   |
+---------------------------------+---------------------------------+
                                  | Produce
                                  v
+-------------------------------------------------------------------+
| 2. Contrato de Transporte (vector_command.hh)                     |
|    - VectorCommand inmutable con CommandKey, PC, VectorConfig     |
|    - Payload desacoplado: Arithmetic / Memory / SlideCommand      |
|    - Normalización escalar canónica (ElementCodec)                |
+---------------------------------+---------------------------------+
                                  | Valida vía requestGrant()
                                  v
+-------------------------------------------------------------------+
| 3. Backend VPU Support (vector_support.cc)                        |
|    - checkStructure: coherencia de registros y alineación LMUL    |
|    - checkCommand / supportsMemory: valida EEW == SEW y perfiles  |
|    - supportsSlide: valida anchos, solapamiento y máscaras        |
|    - Emite RejectionReason ante combinaciones fuera de perfil    |
+-------------------------------------------------------------------+
```

## 4. Huecos de evidencia

Lo implementado que ningún programa ha ejecutado todavía. Sirve de entrada
al cierre del hito (P7), que decidirá qué es requisito:

1. **Anchos 8, 16 y 64 bits** en todas las operaciones enteras, memoria y
   slides. Requieren habilitar SEW en la configuración.
2. **LMUL distinto de m1** (grupos enteros y fraccionales) en todas las
   operaciones.
3. **Formas `.vx`** salvo `vadd.vx`: `vmul.vx`, `vdiv/vrem(u).vx`.
4. **División y resto enteros**, incluidos divisor cero y el caso de
   desbordamiento con signo.
5. **`vfdiv.vv` y la ruta de flags FP**: ningún programa ha producido
   flags IEEE ni ha usado un modo de redondeo distinto de RNE.
6. **`vstart` distinto de 0** y faults de memoria a mitad de instrucción.
7. **Rechazos** (`vm=0`, EEW ≠ SEW, `vd = vs2` en `vslide1up`): sin
   ejecución registrada del camino de rechazo hasta el fault.

[plan-f8]: <Plan de cierre de la fase 8.md>
[fase8]: <Fase 8 - Mínimo entero para Pathfinder.md#evidencia-de-ejecución-de-pathfinder>
[observabilidad]: <Observabilidad - análisis con Pathfinder.md>
[pathfinder]: <../../benchmarks/pathfinder/README.md>
[concurrency]: <../../benchmarks/concurrency/README.md>
[baseline]: <../../benchmarks/baseline/README.md>
