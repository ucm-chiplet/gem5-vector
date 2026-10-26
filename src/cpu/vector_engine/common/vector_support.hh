// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_VECTOR_SUPPORT_HH__
#define __CPU_VECTOR_ENGINE_COMMON_VECTOR_SUPPORT_HH__

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "cpu/vector_engine/common/fp_types.hh"
#include "cpu/vector_engine/common/operation_traits.hh"
#include "cpu/vector_engine/common/vector_types.hh"

namespace gem5::vector_engine
{

struct ArithmeticCommand;
struct MemoryCommand;
struct VectorCommand;
enum class RejectionReason : uint8_t;

/**
 * Política única del soporte funcional implementado. Admisión consulta el
 * comando completo antes de reservar; las unidades usan las mismas reglas
 * como precondiciones y conservan sus comprobaciones locales de protocolo.
 * Structural and architectural validation precede resource admission.
 * Queue availability is deliberately outside this policy.
 */
class VectorSupport
{
  public:
    inline static constexpr auto elementWidths = IntegerElementWidths;

    static bool supportsElementWidth(uint16_t element_bits);
    static bool supportsElementBytes(uint32_t element_bytes);
    static bool supportsConfiguration(const VectorConfig &config);
    static bool supportsIntegerOperation(ArithmeticOperation operation,
                                         uint16_t sew_bits);
    static bool supportsFloatingPointOperation(ArithmeticOperation operation,
                                               uint16_t sew_bits);
    // Capacidades del backend: FloatAdd sólo admite dos fuentes vectoriales.
    static bool supportsArithmetic(const ArithmeticCommand &arithmetic,
                                   uint16_t sew_bits);
    static bool
    supportsExecution(const ArithmeticCommand &arithmetic, uint16_t sew_bits,
                      const std::optional<FpExecutionContext> &fp_context);
    static bool supportsMemory(const MemoryCommand &memory, uint16_t sew_bits);

    // These helpers describe the active widths, not all representable widths.
    static uint32_t elementBytes(uint16_t element_bits);
    static uint64_t elementMask(uint16_t element_bits);

    /**
     * Comprueba el perfil objetivo SEW x LMUL para un ELEN dado. No prueba
     * que existan decode, adaptador de cálculo ni geometría habilitada.
     */
    static bool inTargetProfile(uint16_t sew_bits, VectorLmul lmul,
                                uint16_t elen_bits = 64);
    /**
     * Valida identidad, snapshot y operandos antes de reservar. Devuelve el
     * motivo permanente o nullopt; no consulta colas ni devuelve Retry.
     * Una familia fuera del perfil evita aplicar reglas de grupos ajenas.
     */
    static std::optional<RejectionReason>
    checkStructure(const VectorCommand &command);

    /**
     * Calcula VLMAX aplicando LMUL antes de dividir por SEW. Exige LMUL y
     * ancho representables; no habilita la combinación para ejecutarla.
     */
    static uint64_t maxElements(uint32_t vlen_bytes, VectorLmul lmul,
                                uint16_t sew_bits);

    /**
     * Añade capacidad efectiva y soporte activo a checkStructure. Se llama
     * antes de consultar créditos, incluso con la cola llena. FP desde CPU
     * sigue rechazado; el resultado no indica disponibilidad temporal.
     */
    static std::optional<RejectionReason>
    checkCommand(const VectorCommand &command, uint32_t vlen_bytes,
                 const std::vector<VectorLmul> &supported_lmuls,
                 uint16_t elen_bits = 64);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VECTOR_SUPPORT_HH__
