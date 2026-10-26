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
 * Reúne las reglas para decidir si podemos ejecutar un comando. Primero se
 * revisan sus datos y después las capacidades implementadas y habilitadas.
 * La cola decide por separado si queda espacio: estar llena no convierte
 * un comando incorrecto en uno válido que sólo necesita esperar.
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

    // Tamaño y máscara para anchos ejecutables por las unidades actuales.
    static uint32_t elementBytes(uint16_t element_bits);
    static uint64_t elementMask(uint16_t element_bits);

    /**
     * Indica si el ancho del elemento (SEW) y el tamaño del grupo (LMUL)
     * caben en el perfil que queremos admitir para ese ELEN. Esa combinación
     * puede necesitar trabajo pendiente antes de poder ejecutarse.
     */
    static bool inTargetProfile(uint16_t sew_bits, VectorLmul lmul,
                                uint16_t elen_bits = 64);
    /**
     * Comprueba que el comando tiene identidad, configuración y operandos
     * coherentes antes de reservar espacio. Devuelve el motivo de rechazo
     * o nullopt si pasa esta revisión. No consulta si hay recursos libres.
     */
    static std::optional<RejectionReason>
    checkStructure(const VectorCommand &command);

    /**
     * Calcula cuántos elementos caben en el grupo de registros (VLMAX).
     * LMUL y SEW deben ser representables; el cálculo no comprueba si esta
     * combinación está implementada ni si se ha habilitado al configurar.
     */
    static uint64_t maxElements(uint32_t vlen_bytes, VectorLmul lmul,
                                uint16_t sew_bits);

    /**
     * Revisa además que los elementos quepan y que la operación esté
     * implementada y habilitada. Se llama incluso cuando la cola está llena.
     * Pasar esta revisión no reserva espacio. Los comandos FP de CPU siguen
     * rechazándose hasta implementar su fase correspondiente.
     */
    static std::optional<RejectionReason>
    checkCommand(const VectorCommand &command, uint32_t vlen_bytes,
                 const std::vector<VectorLmul> &supported_lmuls,
                 uint16_t elen_bits = 64);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VECTOR_SUPPORT_HH__
