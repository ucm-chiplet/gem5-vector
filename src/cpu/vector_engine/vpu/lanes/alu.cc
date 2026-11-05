// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/lanes/alu.hh"

#include <algorithm>
#include <initializer_list>
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

/**
 * Interpreta los SEW bits bajos de value como complemento a dos. Se evita
 * convertir un uint64_t mayor que INT64_MAX a int64_t, que en C++17 depende
 * de la implementación: los negativos se calculan como -(~value) - 1.
 */
int64_t
signExtend(uint64_t value, uint16_t bits)
{
    const uint64_t mask = ElementCodec::mask(bits);
    const uint64_t sign = uint64_t{1} << (bits - 1);
    value &= mask;
    if (!(value & sign)) {
        return static_cast<int64_t>(value);
    }
    // ~value & mask es el complemento dentro del elemento y cabe en int64_t.
    return -static_cast<int64_t>(~value & mask) - 1;
}

/** División y resto sin signo; casos de divisor cero según RVV. */
uint64_t
unsignedDivide(ArithmeticOperation operation, uint64_t dividend,
               uint64_t divisor, uint16_t bits)
{
    if (divisor == 0) {
        // Cociente con todos los bits a uno; el resto conserva el dividendo.
        if (operation == ArithmeticOperation::Divide) {
            return ElementCodec::mask(bits);
        }
        return dividend;
    }
    if (operation == ArithmeticOperation::Divide) {
        return dividend / divisor;
    }
    return dividend % divisor;
}

/**
 * División y resto con signo, truncando hacia cero. Divisor cero: cociente
 * -1 (todos los bits a uno) y resto igual al dividendo. Overflow (mínimo
 * representable entre -1): cociente igual al dividendo y resto cero. Este
 * caso se resuelve antes de dividir porque con SEW=64 sería indefinido en
 * el host.
 */
uint64_t
signedDivide(ArithmeticOperation operation, uint64_t dividend_bits,
             uint64_t divisor_bits, uint16_t bits)
{
    const int64_t dividend = signExtend(dividend_bits, bits);
    const int64_t divisor = signExtend(divisor_bits, bits);
    const int64_t minimum = signExtend(uint64_t{1} << (bits - 1), bits);
    const bool is_divide = operation == ArithmeticOperation::Divide;
    if (divisor == 0) {
        if (is_divide) {
            return ElementCodec::mask(bits);
        }
        return dividend_bits;
    }
    if (dividend == minimum && divisor == -1) {
        if (is_divide) {
            return dividend_bits;
        }
        return 0;
    }
    // La conversión de int64_t a uint64_t es módulo 2^64; write conserva
    // después los SEW bits bajos.
    if (is_divide) {
        return static_cast<uint64_t>(dividend / divisor);
    }
    return static_cast<uint64_t>(dividend % divisor);
}

/**
 * Calcula vd = lhs op rhs para un elemento, con lhs = vs2 y rhs = vs1 o
 * el escalar normalizado por CPU. Los operandos llegan con sus SEW bits;
 * el resultado puede tener bits altos, que ElementCodec::write descarta.
 *
 * Suma y multiplicación usan uint64_t, que es aritmética módulo 2^64 sin
 * overflow con signo del host; sus SEW bits bajos no dependen del signo.
 * Referencia: RVV 1.0, edición 20240411, Vector Single-Width Integer Add
 * and Subtract, Vector Single-Width Integer Multiply Instructions y Vector
 * Integer Divide Instructions (casos extremos de la extensión M).
 */
uint64_t
computeInteger(const ExecutionBundle &bundle, uint64_t lhs, uint64_t rhs)
{
    switch (bundle.operation) {
        case ArithmeticOperation::Add:
            return lhs + rhs;
        case ArithmeticOperation::Multiply:
            return lhs * rhs;
        case ArithmeticOperation::Divide:
        case ArithmeticOperation::Remainder:
            if (bundle.signedness == ElementSignedness::Signed) {
                return signedDivide(bundle.operation, lhs, rhs,
                                    bundle.sewBits);
            }
            return unsignedDivide(bundle.operation, lhs, rhs, bundle.sewBits);
        default:
            panic("Unsupported operation in an admitted ALU bundle");
    }
}

/** Posición de un SEW admitido en AluParameters::divideLatency. */
std::size_t
widthIndex(uint16_t sew_bits)
{
    switch (sew_bits) {
        case 8:
            return 0;
        case 16:
            return 1;
        case 32:
            return 2;
        case 64:
            return 3;
        default:
            panic("Invalid element width in an admitted integer bundle");
    }
}

} // anonymous namespace

Alu::Alu(ClockedObject &owner, LaneId lane_id, std::size_t instance,
         ResultSender send_result, statistics::Group &stats_parent,
         const AluParameters &parameters)
    : stats(&stats_parent, "alu" + std::to_string(instance),
            "integer bundles"),
      elements8(&stats, "elements8", statistics::units::Count::get(),
                "Admitted 8-bit integer elements"),
      elements16(&stats, "elements16", statistics::units::Count::get(),
                 "Admitted 16-bit integer elements"),
      elements32(&stats, "elements32", statistics::units::Count::get(),
                 "Admitted 32-bit integer elements"),
      elements64(&stats, "elements64", statistics::units::Count::get(),
                 "Admitted 64-bit integer elements"),
      addElements(&stats, "addElements", statistics::units::Count::get(),
                  "Admitted elements of integer additions"),
      multiplyElements(&stats, "multiplyElements",
                       statistics::units::Count::get(),
                       "Admitted elements of integer multiplications"),
      divideElements(&stats, "divideElements", statistics::units::Count::get(),
                     "Admitted elements of integer divisions"),
      remainderElements(&stats, "remainderElements",
                        statistics::units::Count::get(),
                        "Admitted elements of integer remainders"),
      capacityStalls(&stats, "capacityStalls", statistics::units::Count::get(),
                     "Bundle attempts blocked by execution capacity"),
      intervalStalls(&stats, "intervalStalls", statistics::units::Count::get(),
                     "Bundle attempts blocked by initiation interval or "
                     "an occupied divider"),
      owner(owner),
      parameters(parameters),
      sendResult(std::move(send_result)),
      executeEvent([this] { execute(); },
                   owner.name() + ".lane" + std::to_string(lane_id) + ".alu" +
                       std::to_string(instance),
                   false, VpuResponsePriority)
{
    fatal_if(!parameters.unit.widthBytes ||
                 !parameters.currentProfile(parameters.unit.widthBytes),
             "Invalid timed ALU profile");
    fatal_if(!sendResult, "ALU requires a result receiver");
    fatal_if(parameters.unit.capacity > pending.max_size(),
             "ALU capacity exceeds host container capacity");
    // Validate configured deadlines before accepting any command effects.
    for (const auto &profile : {parameters.add, parameters.multiply}) {
        vpuClockEdge(owner, profile.latency);
        vpuClockEdge(owner, profile.initiationInterval);
    }
    for (const auto latency : parameters.divideLatency) {
        vpuClockEdge(owner, latency);
    }
}

Alu::~Alu()
{
    panic_if(!isIdle(), "Destroying ALU before its work has drained");
}

OperationTiming
Alu::timing(ArithmeticOperation operation, uint16_t sew_bits) const
{
    switch (operation) {
        case ArithmeticOperation::Add:
            return parameters.add;
        case ArithmeticOperation::Multiply:
            return parameters.multiply;
        case ArithmeticOperation::Divide:
        case ArithmeticOperation::Remainder: {
            // Divisor no segmentado: la instancia no admite otro bundle
            // hasta que éste termina.
            const auto latency =
                parameters.divideLatency[widthIndex(sew_bits)];
            return OperationTiming{latency, latency};
        }
        default:
            panic("Unsupported operation in an admitted ALU bundle");
    }
}

void
Alu::countElements(const ExecutionBundle &bundle)
{
    const auto count = bundle.elements.elementCount;
    switch (bundle.sewBits) {
        case 8:
            elements8 += count;
            break;
        case 16:
            elements16 += count;
            break;
        case 32:
            elements32 += count;
            break;
        case 64:
            elements64 += count;
            break;
        default:
            panic("Invalid element width in an admitted integer bundle");
    }
    switch (bundle.operation) {
        case ArithmeticOperation::Add:
            addElements += count;
            break;
        case ArithmeticOperation::Multiply:
            multiplyElements += count;
            break;
        case ArithmeticOperation::Divide:
            divideElements += count;
            break;
        case ArithmeticOperation::Remainder:
            remainderElements += count;
            break;
        default:
            panic("Unsupported operation in an admitted ALU bundle");
    }
}

TransferResult
Alu::acceptBundle(const ExecutionBundle &bundle)
{
    if (pending.size() >= parameters.unit.capacity) {
        ++capacityStalls;
        ++stats.retries;
        return TransferResult::Retry;
    }
    if (curTick() < nextIssue) {
        ++intervalStalls;
        ++stats.retries;
        return TransferResult::Retry;
    }
    panic_if(bundle.fpContext || !VectorSupport::supportsIntegerOperation(
                                     bundle.operation, bundle.sewBits),
             "Unsupported operation in an admitted ALU bundle");
    const auto *traits = operationTraits(bundle.operation);
    panic_if(!traits->acceptsSignedness(bundle.signedness),
             "ALU bundle signedness does not match its operation");
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
             "Invalid integer ALU execution bundle");
    panic_if(bundle.destination.regCount !=
                 VectorSupport::groupRegisterCount(bundle.emul),
             "ALU destination does not match its effective register group");

    panic_if(std::any_of(pending.begin(), pending.end(),
                         [&](const auto &entry) {
                             return entry.bundle.key == bundle.key;
                         }),
             "Duplicate pending execution bundle");
    const auto profile = timing(bundle.operation, bundle.sewBits);
    const auto ready = vpuClockEdge(owner, profile.latency);
    nextIssue = vpuClockEdge(owner, profile.initiationInterval);
    panic_if(ready <= curTick() || nextIssue <= curTick(),
             "Execution timing overflow");
    ExecutionState state;
    state.bundle = bundle;
    state.readyAt = ready;
    countElements(bundle);
    stats.accept(bundle.elements.elementCount, bundle.destinationRange.size);
    // Reservar tantos bytes de resultado como ocupan los elementos recibidos.
    // Conservar identidad y destino para que la lane pueda escribirlos luego.
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
Alu::scheduleNextResult()
{
    if (pending.empty()) {
        return;
    }
    // Un bundle más corto admitido después puede estar listo antes.
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
Alu::execute()
{
    responding = true;
    while (true) {
        // Entregar primero el más antiguo de los listos. Se busca de nuevo
        // tras cada callback, porque la lane puede admitir otro bundle.
        auto ready = std::find_if(
            pending.begin(), pending.end(),
            [](const auto &entry) { return entry.readyAt <= curTick(); });
        if (ready == pending.end()) {
            break;
        }
        auto state = std::move(*ready);
        pending.erase(ready);
        const auto &bundle = state.bundle;
        // Interpretar cada par de elementos usando el orden de bytes del
        // objetivo. lhs es vs2 y rhs es vs1 o el escalar replicado.
        for (std::size_t i = 0; i < bundle.elements.elementCount; ++i) {
            const auto lhs = ElementCodec::read(bundle.lhs, i, bundle.sewBits,
                                                bundle.byteOrder);
            const auto rhs = ElementCodec::read(bundle.rhs, i, bundle.sewBits,
                                                bundle.byteOrder);
            ElementCodec::write(state.result.values, i, bundle.sewBits,
                                computeInteger(bundle, lhs, rhs),
                                bundle.byteOrder);
        }
        auto result = std::move(state.result);
        // Liberar capacidad antes del callback permite una nueva admisión.
        stats.complete();
        sendResult(result);
    }
    responding = false;
    scheduleNextResult();
}

} // namespace gem5::vector_engine
