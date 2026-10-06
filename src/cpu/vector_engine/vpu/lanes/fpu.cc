// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/fpu.hh"

#include <softfloat.h>

#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

namespace
{

uint_fast8_t
softfloatRoundingMode(FpRoundingMode mode)
{
    switch (mode) {
        case FpRoundingMode::NearestEven:
            return softfloat_round_near_even;
        case FpRoundingMode::TowardZero:
            return softfloat_round_minMag;
        case FpRoundingMode::Down:
            return softfloat_round_min;
        case FpRoundingMode::Up:
            return softfloat_round_max;
        case FpRoundingMode::NearestMaxMagnitude:
            return softfloat_round_near_maxMag;
        default:
            panic("Invalid resolved FP rounding mode");
    }
}

/** Aísla el estado global de SoftFloat durante una evaluación sin callbacks.
 */
class SoftFloatScope
{
  public:
    explicit SoftFloatScope(FpRoundingMode mode)
        : savedRounding(softfloat_roundingMode),
          savedTininess(softfloat_detectTininess),
          savedFlags(softfloat_exceptionFlags)
    {
        softfloat_roundingMode = softfloatRoundingMode(mode);
        softfloat_detectTininess = softfloat_tininess_afterRounding;
        softfloat_exceptionFlags = 0;
    }

    ~SoftFloatScope()
    {
        softfloat_roundingMode = savedRounding;
        softfloat_detectTininess = savedTininess;
        softfloat_exceptionFlags = savedFlags;
    }

    SoftFloatScope(const SoftFloatScope &) = delete;
    SoftFloatScope &operator=(const SoftFloatScope &) = delete;

  private:
    const uint_fast8_t savedRounding;
    const uint_fast8_t savedTininess;
    const uint_fast8_t savedFlags;
};

static_assert(FpInexact == softfloat_flag_inexact);
static_assert(FpUnderflow == softfloat_flag_underflow);
static_assert(FpOverflow == softfloat_flag_overflow);
static_assert(FpDivideByZero == softfloat_flag_infinite);
static_assert(FpInvalid == softfloat_flag_invalid);

} // anonymous namespace

Fpu::Fpu(ClockedObject &owner, LaneId lane_id, ResultSender send_result)
    : owner(owner),
      sendResult(std::move(send_result)),
      executeEvent([this] { execute(); },
                   owner.name() + ".lane" + std::to_string(lane_id) + ".fpu")
{
    fatal_if(!sendResult, "FPU requires a result receiver");
}

Fpu::~Fpu()
{
    panic_if(!isIdle(), "Destroying FPU before its work has drained");
}

TransferResult
Fpu::acceptBundle(const ExecutionBundle &bundle)
{
    if (active) {
        return TransferResult::Retry;
    }
    panic_if(!VectorSupport::supportsFloatingPointOperation(bundle.operation,
                                                            bundle.sewBits) ||
                 !bundle.fpContext || !isValid(bundle.fpContext->roundingMode),
             "Unsupported operation or missing FP execution context");
    const auto element_bytes = VectorSupport::elementBytes(bundle.sewBits);
    const uint64_t offset =
        uint64_t{bundle.elements.firstElement} * element_bytes;
    const uint64_t size =
        uint64_t{bundle.elements.elementCount} * element_bytes;
    panic_if(!bundle.key.valid() || !bundle.elements.valid() ||
                 !bundle.destination.naturallyAligned() ||
                 offset + size > std::numeric_limits<uint32_t>::max() ||
                 bundle.destinationRange.offset != offset ||
                 bundle.destinationRange.size != size ||
                 bundle.lhs.size() != bundle.elements.elementCount ||
                 bundle.rhs.size() != bundle.elements.elementCount,
             "Invalid FP16 execution bundle");

    active.emplace();
    active->bundle = bundle;
    active->result =
        ExecutionResult{bundle.key,
                        bundle.sewBits,
                        bundle.elements,
                        bundle.destination,
                        bundle.destinationRange,
                        std::vector<uint32_t>(bundle.elements.elementCount)};
    owner.schedule(executeEvent, owner.clockEdge(Cycles(1)));
    return TransferResult::Accepted;
}

void
Fpu::execute()
{
    panic_if(!active, "FPU evaluation without an accepted bundle");
    const auto &bundle = active->bundle;
    {
        // Restaurar el estado antes del callback evita contaminar la CPU
        // o una ejecución posterior, incluso si el receptor admite trabajo.
        const SoftFloatScope scope(bundle.fpContext->roundingMode);
        for (std::size_t i = 0; i < bundle.lhs.size(); ++i) {
            const float16_t lhs{static_cast<uint16_t>(bundle.lhs[i])};
            const float16_t rhs{static_cast<uint16_t>(bundle.rhs[i])};
            const auto value = f16_add(lhs, rhs).v;
            // RISC-V usa el NaN canónico binary16, incluido el generado
            // por operaciones inválidas. SoftFloat conserva sus flags.
            const bool nan = (value & 0x7c00) == 0x7c00 && (value & 0x03ff);
            active->result.values[i] = nan ? 0x7e00 : value;
        }
        active->result.fpFlags =
            static_cast<FpExceptionFlags>(softfloat_exceptionFlags);
    }
    auto result = std::move(active->result);
    active.reset();
    sendResult(result);
}

} // namespace gem5::vector_engine
