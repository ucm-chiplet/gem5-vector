# Matriz de Decode, Representación y Soporte (Hito 2)

Este documento formaliza la separación exigida en la Fase 2.1 (Punto 3)
y en la sección 2.2 (acción 6) de las especificaciones del Hito 2 entre:
1. **Decode en MinorCPU** (`src/cpu/minor/decode.cc`): qué instrucciones de la
   ISA RISC-V decodifica el frontend de la CPU como offload hacia la VPU.
2. **Representación en el contrato** (`src/cpu/vector_engine/interface/`):
   qué información, tipos de operaciones y variantes de direccionamiento puede
   transportar un `VectorCommand`.
3. **Soporte activo en el backend** (`src/cpu/vector_engine/common/vector_support.cc`):
   qué combinaciones de operación, anchos de elemento (SEW/EEW) y multiplicadores
   de longitud de registro (LMUL) admite y ejecuta la VPU según su perfil.

Adicionalmente, se documenta la distinción entre una ilegalidad normativa
según el estándar RISC-V Vector Extension (RVV 1.0) y un rechazo derivado de
las restricciones de implementación o simplificaciones del modelo VPU en gem5.

---

## 1. Matriz de correspondencia por operación

| Familia / Mnemónico | SEW / EEW en CPU | 1. Decode Minor (`MinorCPU::Decode`) | 2. Representación (`VectorCommand`) | 3. Soporte VPU (`VectorSupport`) | Comportamiento si no se admite |
| :--- | :---: | :--- | :--- | :--- | :--- |
| **Suma entera** (`vadd.vv`, `vadd.vx`) | **8, 16, 32, 64** | Clasificado en `tryDecodeArithmeticOp` como `VpuOffload` (`ArithmeticOperation::Add`). Orden `vd = vs2 + vs1/rs1`. | `ArithmeticCommand` con `operation = Add`, `destination`, `vectorSource = vs2` y `secondOperand` (`VectorRegRef` o `RegVal`). | Admite SEW ∈ {8, 16, 32, 64} y LMUL ∈ {mf8, mf4, mf2, m1, m2, m4, m8} en backend. Requiere forma no enmascarada y `vstart == 0`. | Rechazo con `UnsupportedConfiguration` o `UnsupportedOperation`; MinorCPU eleva `IllegalInstFault`. |
| **Multiplicación entera** (`vmul.vv`, `vmul.vx`) | **8, 16, 32, 64** | Clasificado en `tryDecodeArithmeticOp` como `VpuOffload` (`ArithmeticOperation::Multiply`). Orden `vd = vs2 * vs1/rs1`. | `ArithmeticCommand` con `operation = Multiply`, `signedness = NotApplicable`, `destination`, `vectorSource = vs2` y `secondOperand`. | Admite SEW ∈ {8, 16, 32, 64} y LMUL ∈ {mf8..m8}. Requiere forma no enmascarada y `vstart == 0`. | Rechazo en admisión; MinorCPU eleva `IllegalInstFault`. |
| **División entera** (`vdiv.vv/vx`, `vdivu.vv/vx`) | **8, 16, 32, 64** | Clasificado en `tryDecodeArithmeticOp` como `VpuOffload` (`ArithmeticOperation::Divide`). Orden no conmutativo estricto: `vd = vs2 / vs1/rs1`. | `ArithmeticCommand` con `operation = Divide`, propagando `signedness = Signed` (`vdiv`) o `Unsigned` (`vdivu`). | Admite SEW ∈ {8, 16, 32, 64} y LMUL ∈ {mf8..m8}. División por cero produce cociente según la norma sin generar excepción. | Rechazo en admisión; MinorCPU eleva `IllegalInstFault`. |
| **Resto entero** (`vrem.vv/vx`, `vremu.vv/vx`) | **8, 16, 32, 64** | Clasificado en `tryDecodeArithmeticOp` como `VpuOffload` (`ArithmeticOperation::Remainder`). Orden no conmutativo: `vd = vs2 % vs1/rs1`. | `ArithmeticCommand` con `operation = Remainder`, propagando `signedness = Signed` (`vrem`) o `Unsigned` (`vremu`). | Admite SEW ∈ {8, 16, 32, 64} y LMUL ∈ {mf8..m8}. | Rechazo en admisión; MinorCPU eleva `IllegalInstFault`. |
| **Coma flotante FP32** (`vfadd.vv`, `vfmul.vv`, `vfdiv.vv`) | **32 (FP32)** | Clasificado en `tryDecodeArithmeticOp` como `VpuOffload` (`FloatAdd`, `FloatMultiply`, `FloatDivide`). Verifica `mstatus.FS != Off` antes de despachar. | `ArithmeticCommand` con `fpContext` capturando `frm`. Al retirar actualiza `mstatus.fs = DIRTY` y acumula excepciones en `fflags`. | Admite exclusivamente SEW = 32 (FP32) y LMUL ∈ {mf8..m8}. Requiere redondeos válidos (0..4). | Si `FS=Off` o redondeo inválido, MinorCPU aborta con excepción antes de dispatch. Fuera de SEW=32, rechazo en admisión. |
| **Carga unit-stride** (`vle8/16/32/64.v`) | **EEW ∈ {8, 16, 32, 64}** *(exige SEW = EEW)* | Clasificado en `tryDecodeMemoryOp` como `VpuOffload` (`MemoryDirection::Load`). Extrae EEW de `rv_inst->machInst.width` (8, 16, 32, 64 bits) y registro destino. | `MemoryCommand` con `direction = Load`, `dataReg`, `base` (Addr), `addressing = UnitStrideAddress`, `elementWidthBits = EEW`. | Admite EEW ∈ {8, 16, 32, 64}. **Exige estrictamente `EEW == SEW`** (`elementWidthBits == sew_bits`). | Rechazo con `UnsupportedConfiguration` si `EEW != SEW`; MinorCPU eleva `IllegalInstFault`. |
| **Almacenamiento unit-stride** (`vse8/16/32/64.v`) | **EEW ∈ {8, 16, 32, 64}** *(exige SEW = EEW)* | Clasificado en `tryDecodeMemoryOp` como `VpuOffload` (`MemoryDirection::Store`). Extrae EEW de `rv_inst->machInst.width` (8, 16, 32, 64 bits) y registro fuente. | `MemoryCommand` con `direction = Store`, `dataReg`, `base` (Addr), `addressing = UnitStrideAddress`, `elementWidthBits = EEW`. | Admite EEW ∈ {8, 16, 32, 64}. **Exige estrictamente `EEW == SEW`** (`elementWidthBits == sew_bits`). | Rechazo con `UnsupportedConfiguration` si `EEW != SEW`; MinorCPU eleva `IllegalInstFault`. |
| **Slides de un elemento** (`vslide1up.vx`, `vslide1down.vx`) | **8, 16, 32, 64** | Clasificado en `tryDecodeSlideOp` como `VpuOffload` (`Slide1Up`, `Slide1Down`) sin descomponer la macro-op. Extrae `vd`, `vs2` y referencia a `rs1`. | `SlideCommand` con `operation`, `destination`, `vectorSource = vs2` y `scalar` normalizado con `ElementCodec::normalizeScalar`. | Admite SEW ∈ {8, 16, 32, 64} y LMUL ∈ {mf8..m8}. Solapamiento `vd == vs2` en up y formas con máscara (`vm=0`) rechazadas en admisión. | Solapamiento en up o máscara rechazados en admisión y traducidos por MinorCPU a `IllegalInstFault`. |
| **Mínimo entero con signo** (`vmin.vv`, Pathfinder) | **32 (Pathfinder)** | Clasificado en `tryDecodeArithmeticOp` como `VpuOffload` (`ArithmeticOperation::Minimum`). Orden: `vd = min(vs2, vs1)`. | `ArithmeticCommand` con `operation = Minimum`, `signedness = Signed`, `destination`, `vectorSource = vs2` y `secondOperand = vs1`. | Objetivo Fase 2.8 para Pathfinder con SEW=32. Pendiente de ejecución en backend ALU de la VPU (`alu.cc`). | Pendiente de integración backend VPU. |
| **Memoria strided / indexada** (`vlse`, `luxEI`, etc.) | *Ninguno* | No reconocidas por `VleMacroInst`/`VseMacroInst` unit-stride. Clasificadas como `UnsupportedVector`. | `MemoryAddressing` dispone de variantes `StridedAddress` e `IndexedAddress`. | `VectorSupport::supportsMemory` comprueba y exige exclusivamente `UnitStrideAddress`. | CPU lanza `IllegalInstFault` en Decode. |
| **Otras ops aritméticas y vectoriales** (`vsub`, lógicas, `.vi`, etc.) | *Ninguno* | No reconocidas en Decode. Clasificadas como `UnsupportedVector`. | Variantes existentes en el modelo (`Sub`, `And`, etc.). | Pendientes de fases posteriores o hitos futuros. | MinorCPU eleva `IllegalInstFault` en fase de Decode sin llegar a la VPU. |

---

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
