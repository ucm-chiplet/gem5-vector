// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/common/vector_support.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "cpu/vector_engine/interface/cpu_vector_interface.hh"

namespace gem5::vector_engine
{

bool
VectorSupport::supportsElementWidth(uint16_t element_bits)
{
    return std::find(elementWidths.begin(), elementWidths.end(),
                     element_bits) != elementWidths.end();
}

bool
VectorSupport::supportsElementBytes(uint32_t element_bytes)
{
    return std::any_of(
        elementWidths.begin(), elementWidths.end(),
        [element_bytes](uint16_t bits) { return bits / 8 == element_bytes; });
}

bool
VectorSupport::supportsConfiguration(const VectorConfig &config)
{
    return isValid(config.lmul) && supportsElementWidth(config.sewBits) &&
           !config.masked;
}

bool
VectorSupport::supportsIntegerOperation(ArithmeticOperation operation,
                                        uint16_t sew_bits)
{
    return supportsElementWidth(sew_bits) &&
           operation == ArithmeticOperation::Add;
}

bool
VectorSupport::supportsArithmetic(const ArithmeticCommand &arithmetic,
                                  uint16_t sew_bits)
{
    return supportsIntegerOperation(arithmetic.operation, sew_bits) &&
           arithmetic.widthMode == ElementWidthMode::SameWidth &&
           arithmetic.signedness == ElementSignedness::NotApplicable &&
           (std::holds_alternative<VectorRegRef>(arithmetic.secondOperand) ||
            std::holds_alternative<RegVal>(arithmetic.secondOperand));
}

bool
VectorSupport::supportsMemory(const MemoryCommand &memory, uint16_t sew_bits)
{
    // Sólo EEW=SEW: EMUL=LMUL y todos los rangos usan el mismo ancho.
    return supportsElementWidth(sew_bits) &&
           (memory.direction == MemoryDirection::Load ||
            memory.direction == MemoryDirection::Store) &&
           std::holds_alternative<UnitStrideAddress>(memory.addressing) &&
           memory.elementWidthBits == sew_bits && memory.fieldCount == 1 &&
           !memory.faultOnlyFirst;
}

uint32_t
VectorSupport::elementBytes(uint16_t element_bits)
{
    panic_if(!supportsElementWidth(element_bits),
             "Unsupported vector element width");
    return element_bits / 8;
}

uint32_t
VectorSupport::elementMask(uint16_t element_bits)
{
    panic_if(!supportsElementWidth(element_bits),
             "Unsupported vector element width");
    return std::numeric_limits<uint32_t>::max() >> (32 - element_bits);
}

uint64_t
VectorSupport::maxElements(uint32_t vlen_bytes, VectorLmul lmul,
                           uint16_t sew_bits)
{
    panic_if(!isValid(lmul) || !supportsElementWidth(sew_bits),
             "Invalid vector configuration for VLMAX");
    const int exponent = static_cast<int>(lmul);
    const uint64_t vlen_bits = uint64_t{vlen_bytes} * 8;
    if (exponent >= 0) {
        return (vlen_bits << exponent) / sew_bits;
    }
    return vlen_bits / (uint64_t{sew_bits} << -exponent);
}

std::optional<RejectionReason>
VectorSupport::checkCommand(const VectorCommand &command, uint32_t vlen_bytes,
                            const std::vector<VectorLmul> &supported_lmuls)
{
    const auto &config = command.config;
    if (!supportsConfiguration(config) ||
        std::find(supported_lmuls.begin(), supported_lmuls.end(),
                  config.lmul) == supported_lmuls.end()) {
        return RejectionReason::UnsupportedConfiguration;
    }
    if (config.vl > maxElements(vlen_bytes, config.lmul, config.sewBits)) {
        return RejectionReason::UnsupportedConfiguration;
    }

    // Rango vacío admisible: el sequencer completa sin emitir tareas.
    if (const auto *arithmetic =
            std::get_if<ArithmeticCommand>(&command.payload)) {
        if (!supportsArithmetic(*arithmetic, config.sewBits)) {
            return RejectionReason::UnsupportedOperation;
        }
    } else if (const auto *memory =
                   std::get_if<MemoryCommand>(&command.payload)) {
        if (!supportsMemory(*memory, config.sewBits)) {
            return RejectionReason::UnsupportedConfiguration;
        }
    } else {
        return RejectionReason::InvalidPayload;
    }
    return std::nullopt;
}

} // namespace gem5::vector_engine
