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

| Familia / Mnemónico | 1. Decode Minor (`MinorCPU::Decode`) | 2. Representación (`VectorCommand`) | 3. Soporte VPU (`VectorSupport`) | Comportamiento si no se admite |
| --- | --- | --- | --- | --- |
| **Suma entera** (`vadd.vv`, `vadd.vx`) | Clasificado en `tryDecodeArithmeticOp` como `VpuOffload` (`ArithmeticOperation::Add`). Detecta fuentes vectoriales y escalares (`VecRegClass`, `IntRegClass`). | `ArithmeticCommand` con `operation = Add`, `destination`, `vectorSource` y `secondOperand` (`VectorRegRef` o `RegVal`). | Admite SEW ∈ {8, 16, 32, 64} y LMUL ∈ {mf8, mf4, mf2, m1, m2, m4, m8} en backend. Requiere forma no enmascarada y `vstart == 0`. | Rechazo con `UnsupportedConfiguration` o `UnsupportedOperation`; MinorCPU eleva `IllegalInstFault`. |
| **Carga unit-stride** (`vle8.v`, `vle16.v`, `vle32.v`, `vle64.v`) | Clasificado en `tryDecodeMemoryOp` como `VpuOffload` (`MemoryDirection::Load`). Extrae EEW de `rv_inst->machInst.width` (8, 16, 32, 64 bits) y registro destino. | `MemoryCommand` con `direction = Load`, `dataReg`, `base` (Addr), `addressing = UnitStrideAddress`, `elementWidthBits = EEW`. | Admite EEW ∈ {8, 16, 32, 64}. **Exige estrictamente `EEW == SEW`** (`elementWidthBits == sew_bits`). | Rechazo con `UnsupportedConfiguration` si `EEW != SEW`; MinorCPU eleva `IllegalInstFault`. |
| **Almacenamiento unit-stride** (`vse8.v`, `vse16.v`, `vse32.v`, `vse64.v`) | Clasificado en `tryDecodeMemoryOp` como `VpuOffload` (`MemoryDirection::Store`). Extrae EEW de `rv_inst->machInst.width` (8, 16, 32, 64 bits) y registro fuente. | `MemoryCommand` con `direction = Store`, `dataReg`, `base` (Addr), `addressing = UnitStrideAddress`, `elementWidthBits = EEW`. | Admite EEW ∈ {8, 16, 32, 64}. **Exige estrictamente `EEW == SEW`** (`elementWidthBits == sew_bits`). | Rechazo con `UnsupportedConfiguration` si `EEW != SEW`; MinorCPU eleva `IllegalInstFault`. |
| **Otras ops aritméticas enteras** (`vsub`, `vmul`, lógicas, etc.) | No reconocidas en `tryDecodeArithmeticOp` (sólo se evalúa `vadd_vv` y `vadd_vx`). Clasificadas como `UnsupportedVector`. | `ArithmeticCommand` soporta representar `Sub`, `And`, `Or`, `Xor`, etc. en `ArithmeticOperation`. | Traits implementados en backend según fases avanzadas (Fase 6). | CPU lanza `IllegalInstFault` en fase de Decode sin llegar a la VPU. |
| **Operaciones coma flotante** (`vfadd`, etc.) | Clasificadas como `UnsupportedVector` en Decode. | `VectorCommand` dispone de `fpContext` (`FpExecutionContext`). | Implementadas a nivel de backend para FP32/FP16 (Fase 6); offload desde CPU deshabilitado en fases iniciales. | CPU lanza `IllegalInstFault` en Decode. |
| **Memoria strided / indexada** (`vlse`, `luxEI`, etc.) | No reconocidas por `VleMacroInst`/`VseMacroInst` unit-stride. Clasificadas como `UnsupportedVector`. | `MemoryAddressing` dispone de variantes `StridedAddress` e `IndexedAddress`. | `VectorSupport::supportsMemory` comprueba y exige exclusivamente `UnitStrideAddress`. | CPU lanza `IllegalInstFault` en Decode. |
| **Slides** (`vslide1up.vx`, `vslide1down.vx`) | No reconocidas en el decode actual de MinorCPU (clasificadas como `UnsupportedVector`). | `SlideCommand` define operación, registros fuente/destino y escalar normalizado a SEW. | Backend cuenta con soporte para `Slide` en `VectorSupport::supportsSlide` (Fase 7). | CPU lanza `IllegalInstFault` en Decode. |

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
+---------------------------------+---------------------------------+
                                  | Produce
                                  v
+-------------------------------------------------------------------+
| 2. Contrato de Transporte (vector_command.hh)                     |
|    - VectorCommand inmutable con CommandKey, PC, VectorConfig     |
|    - Payload desacoplado: ArithmeticCommand / MemoryCommand       |
+---------------------------------+---------------------------------+
                                  | Valida vía requestGrant()
                                  v
+-------------------------------------------------------------------+
| 3. Backend VPU Support (vector_support.cc)                        |
|    - checkStructure: coherencia de registros y alineación LMUL    |
|    - checkCommand / supportsMemory: valida EEW == SEW y perfiles  |
|    - Emite RejectionReason ante combinaciones fuera de perfil    |
+-------------------------------------------------------------------+
```
