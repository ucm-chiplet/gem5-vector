// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/fpu.hh"

#include <softfloat.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "cpu/vector_engine/common/element_codec.hh"
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

// NaN canónicos de RISC-V: todo resultado NaN de una operación se sustituye
// por ellos (extensiones F y Zfh, aplicadas también a RVV).
constexpr uint64_t CanonicalNan16 = 0x7e00;
constexpr uint64_t CanonicalNan32 = 0x7fc00000;

bool
isNan16(uint64_t bits)
{
    return (bits & 0x7c00) == 0x7c00 && (bits & 0x03ff);
}

bool
isNan32(uint64_t bits)
{
    return (bits & 0x7f800000) == 0x7f800000 && (bits & 0x007fffff);
}

/**
 * Calcula vd = lhs op rhs para un elemento, con lhs = vs2 y rhs = vs1;
 * en la división, vs2 es el dividendo. Debe llamarse dentro de un
 * SoftFloatScope, que fija el redondeo y acumula los flags del bundle.
 * Referencia: RVV 1.0, edición 20240411, Vector Floating-Point
 * Instructions, Vector Single-Width Floating-Point Add/Subtract
 * Instructions y Vector Single-Width Floating-Point Multiply/Divide
 * Instructions; los resultados siguen IEEE 754 como en la extensión F.
 */
uint64_t
computeFloat(ArithmeticOperation operation, uint16_t sew_bits, uint64_t lhs,
             uint64_t rhs)
{
    if (sew_bits == 16) {
        panic_if(operation != ArithmeticOperation::FloatAdd,
                 "Only the internal binary16 addition is implemented");
        const float16_t a{static_cast<uint16_t>(lhs)};
        const float16_t b{static_cast<uint16_t>(rhs)};
        const uint64_t value = f16_add(a, b).v;
        if (isNan16(value)) {
            return CanonicalNan16;
        }
        return value;
    }
    panic_if(sew_bits != 32, "Unsupported FP element width");
    const float32_t a{static_cast<uint32_t>(lhs)};
    const float32_t b{static_cast<uint32_t>(rhs)};
    uint64_t value = 0;
    switch (operation) {
        case ArithmeticOperation::FloatAdd:
            value = f32_add(a, b).v;
            break;
        case ArithmeticOperation::FloatMultiply:
            value = f32_mul(a, b).v;
            break;
        case ArithmeticOperation::FloatDivide:
            value = f32_div(a, b).v;
            break;
        default:
            panic("Unsupported operation in an admitted FPU bundle");
    }
    if (isNan32(value)) {
        return CanonicalNan32;
    }
    return value;
}

} // anonymous namespace

Fpu::Fpu(ClockedObject &owner, LaneId lane_id, ResultSender send_result,
         statistics::Group &stats_parent, const FpuParameters &parameters)
    : stats(&stats_parent, "fpu", "floating-point bundles"),
      add16Elements(&stats, "add16Elements", statistics::units::Count::get(),
                    "Admitted elements of internal binary16 additions"),
      add32Elements(&stats, "add32Elements", statistics::units::Count::get(),
                    "Admitted elements of binary32 additions"),
      multiply32Elements(&stats, "multiply32Elements",
                         statistics::units::Count::get(),
                         "Admitted elements of binary32 multiplications"),
      divide32Elements(&stats, "divide32Elements",
                       statistics::units::Count::get(),
                       "Admitted elements of binary32 divisions"),
      inexactBundles(&stats, "inexactBundles", statistics::units::Count::get(),
                     "Completed bundles reporting inexact"),
      underflowBundles(&stats, "underflowBundles",
                       statistics::units::Count::get(),
                       "Completed bundles reporting underflow"),
      overflowBundles(&stats, "overflowBundles",
                      statistics::units::Count::get(),
                      "Completed bundles reporting overflow"),
      divideByZeroBundles(&stats, "divideByZeroBundles",
                          statistics::units::Count::get(),
                          "Completed bundles reporting divide by zero"),
      invalidBundles(&stats, "invalidBundles", statistics::units::Count::get(),
                     "Completed bundles reporting invalid operation"),
      capacityStalls(&stats, "capacityStalls", statistics::units::Count::get(),
                     "Add/multiply attempts blocked by pipeline capacity"),
      intervalStalls(&stats, "intervalStalls", statistics::units::Count::get(),
                     "Add/multiply attempts blocked by initiation interval"),
      dividerStalls(&stats, "dividerStalls", statistics::units::Count::get(),
                    "Divide attempts blocked by the occupied divider"),
      owner(owner),
      parameters(parameters),
      sendResult(std::move(send_result)),
      executeEvent([this] { execute(); },
                   owner.name() + ".lane" + std::to_string(lane_id) + ".fpu",
                   false, VpuResponsePriority)
{
    fatal_if(!parameters.unit.widthBytes ||
                 !parameters.currentProfile(parameters.unit.widthBytes),
             "Invalid timed FPU profile");
    fatal_if(!sendResult, "FPU requires a result receiver");
    fatal_if(uint64_t{parameters.unit.capacity} + 1 > pending.max_size(),
             "FPU capacity exceeds host container capacity");
    vpuClockEdge(owner, parameters.addMultiply.latency);
    vpuClockEdge(owner, parameters.addMultiply.initiationInterval);
    vpuClockEdge(owner, parameters.divideLatency);
}

Fpu::~Fpu()
{
    panic_if(!isIdle(), "Destroying FPU before its work has drained");
}

void
Fpu::validateBundle(const ExecutionBundle &bundle) const
{
    panic_if(!VectorSupport::supportsFloatingPointOperation(bundle.operation,
                                                            bundle.sewBits) ||
                 bundle.signedness != ElementSignedness::NotApplicable ||
                 !bundle.fpContext || !isValid(bundle.fpContext->roundingMode),
             "Unsupported operation or missing FP execution context");
    const auto element_bytes = VectorSupport::elementBytes(bundle.sewBits);
    const uint64_t offset =
        uint64_t{bundle.elements.firstElement} * element_bytes;
    const uint64_t size =
        uint64_t{bundle.elements.elementCount} * element_bytes;
    panic_if(!bundle.key.valid() || !bundle.elements.valid() ||
                 !isValid(bundle.emul) ||
                 !bundle.destination.naturallyAligned() ||
                 offset + size > std::numeric_limits<uint32_t>::max() ||
                 bundle.destinationRange.offset != offset ||
                 bundle.destinationRange.size != size ||
                 size > parameters.unit.widthBytes ||
                 bundle.lhs.size() != size || bundle.rhs.size() != size ||
                 (bundle.byteOrder != ByteOrder::little &&
                  bundle.byteOrder != ByteOrder::big),
             "Invalid FP execution bundle");
    panic_if(std::any_of(pending.begin(), pending.end(),
                         [&](const auto &entry) {
                             return entry.bundle.key == bundle.key;
                         }),
             "Duplicate pending execution bundle");
}

void
Fpu::countElements(const ExecutionBundle &bundle)
{
    const auto count = bundle.elements.elementCount;
    switch (bundle.operation) {
        case ArithmeticOperation::FloatAdd:
            if (bundle.sewBits == 16) {
                add16Elements += count;
            } else {
                add32Elements += count;
            }
            break;
        case ArithmeticOperation::FloatMultiply:
            multiply32Elements += count;
            break;
        case ArithmeticOperation::FloatDivide:
            divide32Elements += count;
            break;
        default:
            panic("Unsupported operation in an admitted FPU bundle");
    }
}

TransferResult
Fpu::acceptBundle(const ExecutionBundle &bundle)
{
    // Cada subrecurso aplica su propia ocupación; uno lleno no rechaza
    // trabajo destinado al otro.
    const bool divider = bundle.operation == ArithmeticOperation::FloatDivide;
    if (divider) {
        if (dividerBusy) {
            ++dividerStalls;
            ++stats.retries;
            return TransferResult::Retry;
        }
    } else {
        if (pipelineEntries >= parameters.unit.capacity) {
            ++capacityStalls;
            ++stats.retries;
            return TransferResult::Retry;
        }
        if (curTick() < pipelineNextIssue) {
            ++intervalStalls;
            ++stats.retries;
            return TransferResult::Retry;
        }
    }
    validateBundle(bundle);

    Tick ready = 0;
    if (divider) {
        ready = vpuClockEdge(owner, parameters.divideLatency);
        dividerBusy = true;
    } else {
        ready = vpuClockEdge(owner, parameters.addMultiply.latency);
        pipelineNextIssue =
            vpuClockEdge(owner, parameters.addMultiply.initiationInterval);
        ++pipelineEntries;
    }
    panic_if(ready <= curTick(), "Execution timing overflow");
    const auto size = bundle.destinationRange.size;
    ExecutionState state;
    state.bundle = bundle;
    state.readyAt = ready;
    state.divider = divider;
    countElements(bundle);
    stats.accept(bundle.elements.elementCount, size);
    state.result = ExecutionResult{bundle.key,
                                   bundle.sewBits,
                                   bundle.elements,
                                   bundle.destination,
                                   bundle.destinationRange,
                                   ByteBuffer(size),
                                   0,
                                   bundle.byteOrder,
                                   bundle.emul};
    pending.push_back(std::move(state));
    if (!responding) {
        scheduleNextResult();
    }
    return TransferResult::Accepted;
}

void
Fpu::compute(ExecutionState &state) const
{
    const auto &bundle = state.bundle;
    // Restaurar el estado antes del callback evita contaminar la CPU o una
    // ejecución posterior, incluso si el receptor admite trabajo. Los flags
    // reúnen sólo los elementos de este bundle, todos ellos ejecutados.
    const SoftFloatScope scope(bundle.fpContext->roundingMode);
    for (std::size_t i = 0; i < bundle.elements.elementCount; ++i) {
        const auto lhs = ElementCodec::read(bundle.lhs, i, bundle.sewBits,
                                            bundle.byteOrder);
        const auto rhs = ElementCodec::read(bundle.rhs, i, bundle.sewBits,
                                            bundle.byteOrder);
        ElementCodec::write(
            state.result.values, i, bundle.sewBits,
            computeFloat(bundle.operation, bundle.sewBits, lhs, rhs),
            bundle.byteOrder);
    }
    state.result.fpFlags =
        static_cast<FpExceptionFlags>(softfloat_exceptionFlags);
}

void
Fpu::scheduleNextResult()
{
    if (pending.empty()) {
        return;
    }
    const auto earliest = std::min_element(
        pending.begin(), pending.end(),
        [](const auto &a, const auto &b) { return a.readyAt < b.readyAt; });
    if (executeEvent.scheduled()) {
        if (executeEvent.when() <= earliest->readyAt) {
            return;
        }
        owner.deschedule(executeEvent);
    }
    owner.schedule(executeEvent, std::max(earliest->readyAt, curTick()));
}

void
Fpu::execute()
{
    responding = true;
    while (true) {
        // El más antiguo de los listos; se busca de nuevo tras cada callback.
        auto ready = std::find_if(
            pending.begin(), pending.end(),
            [](const auto &entry) { return entry.readyAt <= curTick(); });
        if (ready == pending.end()) {
            break;
        }
        auto state = std::move(*ready);
        pending.erase(ready);
        compute(state);
        const auto flags = state.result.fpFlags;
        inexactBundles += bool(flags & FpInexact);
        underflowBundles += bool(flags & FpUnderflow);
        overflowBundles += bool(flags & FpOverflow);
        divideByZeroBundles += bool(flags & FpDivideByZero);
        invalidBundles += bool(flags & FpInvalid);
        // Liberar el subrecurso antes del callback permite una admisión.
        if (state.divider) {
            dividerBusy = false;
        } else {
            --pipelineEntries;
        }
        auto result = std::move(state.result);
        stats.complete();
        sendResult(result);
    }
    responding = false;
    scheduleNextResult();
}

} // namespace gem5::vector_engine
