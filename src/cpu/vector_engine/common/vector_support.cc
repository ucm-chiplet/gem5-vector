// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/common/vector_support.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "cpu/vector_engine/common/element_codec.hh"
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
    return !config.illegalVtype && isValid(config.lmul) &&
           supportsElementWidth(config.sewBits) && !config.masked;
}

bool
VectorSupport::supportsIntegerOperation(ArithmeticOperation operation,
                                        uint16_t sew_bits)
{
    const auto *traits = operationTraits(operation);
    return traits && traits->execution == ExecutionClass::IntegerAlu &&
           traits->supportsWidth(sew_bits);
}

bool
VectorSupport::supportsFloatingPointOperation(ArithmeticOperation operation,
                                              uint16_t sew_bits)
{
    const auto *traits = operationTraits(operation);
    return traits && traits->execution == ExecutionClass::FloatingPoint &&
           traits->supportsWidth(sew_bits);
}

bool
VectorSupport::supportsArithmetic(const ArithmeticCommand &arithmetic,
                                  uint16_t sew_bits)
{
    if (arithmetic.widthMode != ElementWidthMode::SameWidth ||
        arithmetic.signedness != ElementSignedness::NotApplicable) {
        return false;
    }
    const bool vector_source =
        std::holds_alternative<VectorRegRef>(arithmetic.secondOperand);
    const auto *traits = operationTraits(arithmetic.operation);
    return traits && traits->supportsWidth(sew_bits) &&
           ((vector_source && traits->vectorVector) ||
            (std::holds_alternative<RegVal>(arithmetic.secondOperand) &&
             traits->vectorScalar));
}

bool
VectorSupport::supportsExecution(
    const ArithmeticCommand &arithmetic, uint16_t sew_bits,
    const std::optional<FpExecutionContext> &fp_context)
{
    if (!supportsArithmetic(arithmetic, sew_bits)) {
        return false;
    }
    if (supportsIntegerOperation(arithmetic.operation, sew_bits)) {
        return !fp_context;
    }
    return fp_context && isValid(fp_context->roundingMode);
}

bool
VectorSupport::supportsMemory(const MemoryCommand &memory, uint16_t sew_bits)
{
    // Sólo EEW=SEW: EMUL=LMUL y todos los rangos usan el mismo ancho.
    const auto *traits = operationTraits(memory.direction);
    return traits && traits->supportsWidth(sew_bits) &&
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

uint64_t
VectorSupport::elementMask(uint16_t element_bits)
{
    panic_if(!supportsElementWidth(element_bits),
             "Unsupported vector element width");
    return ElementCodec::mask(element_bits);
}

bool
VectorSupport::inTargetProfile(uint16_t sew_bits, VectorLmul lmul,
                               uint16_t elen_bits)
{
    if (!ElementCodec::validWidth(sew_bits) || !isValid(lmul) ||
        !ElementCodec::validWidth(elen_bits) || sew_bits > elen_bits) {
        return false;
    }
    const int exponent = static_cast<int>(lmul);
    if (exponent >= 0) {
        return true;
    }
    // Con un grupo fraccional, reducir también el máximo ancho admitido.
    return sew_bits <= (elen_bits >> -exponent);
}

std::optional<RejectionReason>
VectorSupport::checkStructure(const VectorCommand &command)
{
    // Sin identidad o configuración coherente no podemos seguir el comando.
    if (!command.command.valid()) {
        return RejectionReason::InvalidIdentity;
    }
    const auto &config = command.config;
    if (config.illegalVtype || !isValid(config.lmul) ||
        !ElementCodec::validWidth(config.sewBits) ||
        config.vl > std::numeric_limits<uint32_t>::max() ||
        config.vstart > std::numeric_limits<uint32_t>::max()) {
        return RejectionReason::InvalidVectorConfig;
    }
    if (command.payload.valueless_by_exception() ||
        command.requestorId == Request::invldRequestorId) {
        return RejectionReason::InvalidPayload;
    }
    // Un grupo fraccional sigue usando un nombre de registro. Los grupos
    // mayores que m1 ocupan 2, 4 u 8 registros consecutivos.
    unsigned expected_regs = 1;
    if (config.lmul > VectorLmul::M1) {
        expected_regs = 1U << static_cast<int>(config.lmul);
    }
    const auto check_reg = [&](const VectorRegRef &reg) {
        return reg.naturallyAligned() && reg.regCount == expected_regs;
    };
    if (const auto *arithmetic =
            std::get_if<ArithmeticCommand>(&command.payload)) {
        // Las operaciones con anchos distintos tendrían otras reglas de
        // grupos. Rechazarlas antes de aplicar las del perfil actual.
        if (arithmetic->widthMode != ElementWidthMode::SameWidth ||
            arithmetic->signedness != ElementSignedness::NotApplicable) {
            return RejectionReason::UnsupportedOperation;
        }
        if (!check_reg(arithmetic->destination) ||
            !check_reg(arithmetic->vectorSource)) {
            return RejectionReason::InvalidRegisterGroup;
        }
        if (const auto *reg =
                std::get_if<VectorRegRef>(&arithmetic->secondOperand);
            reg && !check_reg(*reg)) {
            return RejectionReason::InvalidRegisterGroup;
        }
        if (arithmetic->secondOperand.valueless_by_exception() ||
            arithmetic->operation == ArithmeticOperation::Invalid) {
            return RejectionReason::InvalidPayload;
        }
    } else {
        const auto &memory = std::get<MemoryCommand>(command.payload);
        // Primero revisar que los campos tienen sentido; después comprobar
        // que pertenecen al perfil de memoria que sabemos ejecutar.
        if (!operationTraits(memory.direction) ||
            memory.addressing.valueless_by_exception() ||
            !ElementCodec::validWidth(memory.elementWidthBits) ||
            memory.fieldCount == 0 || memory.fieldCount > 8) {
            return RejectionReason::InvalidPayload;
        }
        if (memory.elementWidthBits != config.sewBits ||
            memory.fieldCount != 1) {
            return RejectionReason::UnsupportedConfiguration;
        }
        if (!check_reg(memory.dataReg)) {
            return RejectionReason::InvalidRegisterGroup;
        }
    }
    return std::nullopt;
}

uint64_t
VectorSupport::maxElements(uint32_t vlen_bytes, VectorLmul lmul,
                           uint16_t sew_bits)
{
    panic_if(!isValid(lmul) || !ElementCodec::validWidth(sew_bits),
             "Invalid vector configuration for VLMAX");
    const int exponent = static_cast<int>(lmul);
    const uint64_t vlen_bits = uint64_t{vlen_bytes} * 8;
    // LMUL amplía o reduce el espacio disponible antes de contar elementos.
    if (exponent >= 0) {
        return (vlen_bits << exponent) / sew_bits;
    }
    return vlen_bits / (uint64_t{sew_bits} << -exponent);
}

std::optional<RejectionReason>
VectorSupport::checkCommand(const VectorCommand &command, uint32_t vlen_bytes,
                            const std::vector<VectorLmul> &supported_lmuls,
                            uint16_t elen_bits)
{
    // Mantener el orden: datos coherentes, espacio en registros y soporte.
    // Así el motivo de rechazo no depende de la ocupación de la cola.
    if (const auto reason = checkStructure(command)) {
        return reason;
    }
    const auto &config = command.config;
    if (config.vl > maxElements(vlen_bytes, config.lmul, config.sewBits)) {
        return RejectionReason::InvalidVectorConfig;
    }
    if (!supportsConfiguration(config) ||
        !inTargetProfile(config.sewBits, config.lmul, elen_bits) ||
        std::find(supported_lmuls.begin(), supported_lmuls.end(),
                  config.lmul) == supported_lmuls.end()) {
        return RejectionReason::UnsupportedConfiguration;
    }

    // Rango vacío admisible: el sequencer completa sin emitir tareas.
    if (const auto *arithmetic =
            std::get_if<ArithmeticCommand>(&command.payload)) {
        const auto *traits = operationTraits(arithmetic->operation);
        // Tener campos para redondeo y flags no habilita todavía FP desde CPU.
        if (!traits || !traits->cpuOffload ||
            !supportsExecution(*arithmetic, config.sewBits,
                               command.fpContext)) {
            return RejectionReason::UnsupportedOperation;
        }
    } else if (const auto *memory =
                   std::get_if<MemoryCommand>(&command.payload)) {
        if (command.fpContext || !supportsMemory(*memory, config.sewBits)) {
            return RejectionReason::UnsupportedConfiguration;
        }
    } else {
        return RejectionReason::InvalidPayload;
    }
    return std::nullopt;
}

} // namespace gem5::vector_engine
