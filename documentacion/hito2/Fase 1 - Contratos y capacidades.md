# Fase 1 del hito 2: contratos y capacidades

Implementación autorizada por el usuario el 26 de octubre de 2026, sobre
`5fc5c12f1047`. Esta entrega atiende O2 (modelo con recursos finitos) y O6
(contratos y resultados trazables). Prepara las comparaciones de O3; no
implementa modelos tecnológicos, IME ni organizaciones alternativas.

El baseline del hito 1 permanece cerrado. Esta fase adapta representación,
validación, construcción y cierre; no añade instrucciones ni acredita los
resultados funcionales o temporales de las fases siguientes.

## Semántica, capacidades y datos

`describeOperands` produce una vista derivada de roles, acceso, formato,
EEW y EMUL. No almacena una copia mutable paralela a los operandos del
comando. El store identifica una fuente; la carga, un destino. Los roles
de acumulador/predicado y destinos escalares son puntos de extensión, sin
operaciones activas que los utilicen. Ampliar las fuentes de una operación
requiere ampliar su descriptor y validación; no basta añadir un rol al enum.

## Referencia normativa y comprobaciones

Autoridad: RVV 1.0, manual oficial **edición 20240411**:
[capítulo vectorial][rvv]. Secciones consultadas: *Vector Register Grouping*,
*Vector Operands*, *Mapping for LMUL < 1*, *Vector Tail Agnostic and Vector
Mask Agnostic*, *Vector Start Index CSR*, *Prestart, Active, Inactive, Body,
and Tail Element Definitions*, *Vector Arithmetic Instruction encoding*,
*Vector Single-Width Integer Add and Subtract*, *Vector Load/Store Width
Encoding*, *Vector Unit-Stride Instructions* y *Vector Floating-Point
Exception Flags*. Los tiempos y la política física son decisiones del
modelo, no requisitos temporales RVV.

[rvv]: https://riscv.github.io/riscv-unified-db/manual/html/isa/isa_20240411/chapters/v-st-ext.html
