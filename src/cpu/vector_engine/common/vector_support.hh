// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_VECTOR_SUPPORT_HH__
#define __CPU_VECTOR_ENGINE_COMMON_VECTOR_SUPPORT_HH__

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

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
 * No valida identidades, referencias arquitectónicas ni capacidad de colas.
 */
class VectorSupport
{
  public:
    inline static constexpr std::array<uint16_t, 2> elementWidths{16, 32};

    static bool supportsElementWidth(uint16_t element_bits);
    static bool supportsElementBytes(uint32_t element_bytes);
    static bool supportsConfiguration(const VectorConfig &config);
    static bool supportsIntegerOperation(ArithmeticOperation operation,
                                         uint16_t sew_bits);
    static bool supportsArithmetic(const ArithmeticCommand &arithmetic,
                                   uint16_t sew_bits);
    static bool supportsMemory(const MemoryCommand &memory, uint16_t sew_bits);

    // Precondición: ancho soportado. La máscara ajusta un patrón de bits
    // al SEW activo; uint32_t es el contenedor, no el ancho del elemento.
    static uint32_t elementBytes(uint16_t element_bits);
    static uint32_t elementMask(uint16_t element_bits);

    // Precondición: LMUL válido y SEW soportado; aplica LMUL antes de dividir.
    static uint64_t maxElements(uint32_t vlen_bytes, VectorLmul lmul,
                                uint16_t sew_bits);

    // Precondición: descriptor validado estructuralmente por la frontera CPU.
    static std::optional<RejectionReason>
    checkCommand(const VectorCommand &command, uint32_t vlen_bytes,
                 const std::vector<VectorLmul> &supported_lmuls);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VECTOR_SUPPORT_HH__
