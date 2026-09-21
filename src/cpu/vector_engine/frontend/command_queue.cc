/* SPDX-License-Identifier: BSD-3-Clause */

#include "cpu/vector_engine/frontend/command_queue.hh"

#include <algorithm>

#include "base/logging.hh"

namespace gem5::vector_engine
{

CommandQueue::CommandQueue(
    std::size_t queue_depth,
    uint32_t vlen_bytes,
    const std::vector<VectorLmul> &supported_lmuls,
    CpuCompletionEndpoint &completion_endpoint)
    : queueDepth(queue_depth),
      vlenBytes(vlen_bytes),
      supportedLmuls(supported_lmuls),
      completionEndpoint(completion_endpoint)
{
    // Errores estáticos de configuración: no son backpressure ni rechazos
    // de instrucciones. VectorEngine validará además la geometría global
    // y la coincidencia de VLEN con Minor cuando se integre el coprocesador.
    fatal_if(queueDepth == 0,
             "CommandQueue requires at least one entry");
    fatal_if(vlenBytes == 0,
             "CommandQueue requires a nonzero VLEN");
    fatal_if(supportedLmuls.empty(),
             "CommandQueue requires at least one supported LMUL");

    for (auto it = supportedLmuls.begin();
         it != supportedLmuls.end(); ++it) {
        fatal_if(!isValid(*it), "Invalid supported LMUL");
        fatal_if(std::find(supportedLmuls.begin(), it, *it) != it,
                 "Duplicate supported LMUL");
        fatal_if(maxElements(*it) == 0,
                 "Supported LMUL cannot hold a 32-bit element");
    }
}

uint64_t
CommandQueue::maxElements(VectorLmul lmul) const
{
    // LMUL codifica un exponente de dos. Se aplica antes de dividir para
    // evitar truncar VLEN/SEW prematuramente; no se usa coma flotante.
    // Los 64 bits contienen VLEN en bits y el mayor LMUL representable.
    const int exponent = static_cast<int>(lmul);
    const uint64_t vlen_bits = uint64_t{vlenBytes} * 8;

    if (exponent >= 0)
        return (vlen_bits << exponent) / 32;

    return vlen_bits / (uint64_t{32} << -exponent);
}

std::optional<RejectionReason>
CommandQueue::checkSupport(const VectorCommand &command) const
{
    // La interfaz CPU ya ha validado identidad, enums, alternativas,
    // grupos LMUL/EMUL y metadatos. Aquí se filtra el soporte implementado.
    const auto &config = command.config;

    const bool supported_lmul =
        std::find(supportedLmuls.begin(), supportedLmuls.end(),
                  config.lmul) != supportedLmuls.end();

    if (config.sewBits != 32 || config.masked || !supported_lmul)
        return RejectionReason::UnsupportedConfiguration;

    if (config.vl > maxElements(config.lmul))
        return RejectionReason::UnsupportedConfiguration;

    // vl == 0 y vstart >= vl son admisibles. El sequencer los completará
    // sin crear tareas; admisión no ejecuta ni finaliza esos comandos.
    if (const auto *arithmetic =
            std::get_if<ArithmeticCommand>(&command.payload)) {
        if (arithmetic->operation != ArithmeticOperation::Add ||
            arithmetic->widthMode != ElementWidthMode::SameWidth ||
            arithmetic->signedness !=
                ElementSignedness::NotApplicable ||
            std::holds_alternative<int64_t>(
                arithmetic->secondOperand)) {
            return RejectionReason::UnsupportedOperation;
        }
    } else if (const auto *memory =
                   std::get_if<MemoryCommand>(&command.payload)) {
        if (!std::holds_alternative<UnitStrideAddress>(
                memory->addressing) ||
            memory->elementWidthBits != 32 ||
            memory->fieldCount != 1 ||
            memory->faultOnlyFirst) {
            return RejectionReason::UnsupportedConfiguration;
        }
    } else {
        return RejectionReason::InvalidPayload;
    }

    return std::nullopt;
}

bool
CommandQueue::contains(CommandKey command) const
{
    // La clave completa incluye el contexto. Se recorre el estado vivo
    // para no mantener una segunda tabla que deba sincronizarse con él.
    for (const auto &entry : state.reservations) {
        if (entry.second.descriptor.command == command)
            return true;
    }

    for (const auto &queued : state.commands) {
        if (queued.command == command)
            return true;
    }

    return false;
}

GrantResult
CommandQueue::requestGrant(const VectorCommand &command)
{
    // Los rechazos permanentes preceden al examen de capacidad: una cola
    // llena no debe convertir un descriptor no soportado en un Stall.
    if (const auto reason = checkSupport(command))
        return GrantResult::rejected(*reason);

    if (contains(command.command))
        return GrantResult::rejected(RejectionReason::DuplicateCommand);

    panic_if(occupancy() > queueDepth,
             "CommandQueue capacity invariant violated");

    // Stall no crea estado ni consume un ID. El emisor conserva el mismo
    // descriptor para reintentarlo cuando pueda iniciar un offload.
    if (state.draining || full())
        return GrantResult::stall();

    // El último valor queda reservado como inválido. No se permite que
    // el contador desborde y vuelva a hacer válido un token antiguo.
    panic_if(
        state.nextReservationId == GrantToken::InvalidReservationId,
        "CommandQueue reservation IDs exhausted");

    const GrantToken token{
        state.nextReservationId,
        command.command
    };

    const auto result = state.reservations.emplace(
        token.reservationId,
        detail::CommandReservation{token, command});

    panic_if(!result.second, "Duplicate reservation ID");

    ++state.nextReservationId;

    // Sólo se reserva capacidad: aún no hay entrada FIFO ni accepted.
    return GrantResult::granted(token);
}

void
CommandQueue::dispatch(
    const GrantToken &token,
    const VectorCommand &command)
{
    panic_if(!token.valid(), "Invalid grant token");

    const auto reservation =
        state.reservations.find(token.reservationId);

    panic_if(reservation == state.reservations.end(),
             "Unknown or already consumed grant token");

    panic_if(reservation->second.token != token,
             "Grant token does not match its reservation");

    panic_if(reservation->second.descriptor != command,
             "Dispatched command differs from reserved command");

    const CommandKey accepted_key =
        reservation->second.descriptor.command;

    // La plaza ya está reservada: no se consulta full() ni draining.
    // Primero se copia el descriptor y después se consume la reserva.
    // No hay callbacks entre ambas operaciones; la ocupación observable
    // se conserva y nunca se pierde una reserva antes de guardar el comando.
    state.commands.push_back(reservation->second.descriptor);
    state.reservations.erase(reservation);

    // El receptor puede ejecutar código síncrono. Todo el estado debe
    // estar preparado antes del callback y no se modifica después de él.
    completionEndpoint.accepted(accepted_key);
}

std::optional<VectorCommand>
CommandQueue::front() const
{
    if (empty())
        return std::nullopt;

    return state.commands.front();
}

void
CommandQueue::releaseHead(CommandKey command)
{
    panic_if(empty(), "Cannot release an empty CommandQueue");

    panic_if(state.commands.front().command != command,
             "Completion does not match CommandQueue head");

    // Sólo el cierre del comando libera su crédito. Retirarlo al iniciar
    // ejecución permitiría admitir más trabajo del que cuenta la capacidad.
    // La notificación completed corresponde al sequencer y a la interfaz.
    state.commands.pop_front();
}

void
CommandQueue::endDrain()
{
    panic_if(!isIdle(),
             "Cannot finish CommandQueue drain with pending work");

    state.draining = false;
}

} // namespace gem5::vector_engine
