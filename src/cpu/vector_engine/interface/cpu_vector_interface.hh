#ifndef __CPU_VECTOR_ENGINE_INTERFACE_CPU_VECTOR_INTERFACE_HH__
#define __CPU_VECTOR_ENGINE_INTERFACE_CPU_VECTOR_INTERFACE_HH__

#include <cstdint>
#include <limits>
#include <optional>

#include "cpu/vector_engine/common/command_key.hh"
#include "cpu/vector_engine/interface/vector_command.hh"
#include "cpu/vector_engine/interface/vector_completion.hh"

namespace gem5::vector_engine
{

/** Admisión indica a MinorCPU si reserva espacio, debe esperar o rechaza. */
enum class GrantStatus : uint8_t
{
    Granted,
    Stall,
    Rejected,
};

/**
 * Motivo que admisión devuelve antes de aceptar un comando.
 * MinorCPU lo usa para tratar el rechazo, sin esperar una finalización.
 */
enum class RejectionReason : uint8_t
{
    InvalidIdentity,
    InvalidVectorConfig,
    InvalidRegisterGroup,
    InvalidPayload,
    UnsupportedOperation,
    UnsupportedConfiguration,
    DuplicateCommand,
};

/**
 * Reserva que admisión concede a MinorCPU para un comando concreto.
 * MinorCPU la entrega en dispatch; admisión la consume una sola vez.
 */
struct GrantToken
{
    static constexpr uint64_t InvalidReservationId =
        std::numeric_limits<uint64_t>::max();

    uint64_t reservationId = InvalidReservationId;
    CommandKey command;

    constexpr bool
    valid() const
    {
        return reservationId != InvalidReservationId && command.valid();
    }
};

constexpr bool
operator==(const GrantToken &lhs, const GrantToken &rhs)
{
    return lhs.reservationId == rhs.reservationId &&
           lhs.command == rhs.command;
}

constexpr bool
operator!=(const GrantToken &lhs, const GrantToken &rhs)
{
    return !(lhs == rhs);
}

/**
 * Respuesta de requestGrant que admisión devuelve a MinorCPU.
 * Granted lleva un token; Rejected, un motivo; Stall no lleva ninguno.
 * Conceder la reserva todavía no inserta el comando en la FIFO.
 */
struct GrantResult
{
    GrantStatus status = GrantStatus::Stall;
    std::optional<GrantToken> token;
    std::optional<RejectionReason> rejectionReason;

    static GrantResult
    granted(GrantToken granted_token)
    {
        return {GrantStatus::Granted, granted_token, std::nullopt};
    }

    static GrantResult
    stall()
    {
        return {GrantStatus::Stall, std::nullopt, std::nullopt};
    }

    static GrantResult
    rejected(RejectionReason reason)
    {
        return {GrantStatus::Rejected, std::nullopt, reason};
    }

    bool
    consistent() const
    {
        switch (status) {
          case GrantStatus::Granted:
            return token.has_value() && token->valid() &&
                   !rejectionReason.has_value();
          case GrantStatus::Stall:
            return !token.has_value() && !rejectionReason.has_value();
          case GrantStatus::Rejected:
            return !token.has_value() && rejectionReason.has_value();
        }

        return false;
    }
};

/**
 * Contrato de entrada que implementará la admisión de la VPU.
 * MinorCPU lo usa para reservar y entregar comandos por CpuVectorInterface.
 * Las referencias recibidas no transfieren la propiedad de los objetos.
 */
class VpuCommandEndpoint
{
  public:
    virtual ~VpuCommandEndpoint() = default;

    // MinorCPU pide una reserva; Stall permite reintentar el mismo comando.
    virtual GrantResult requestGrant(const VectorCommand &command) = 0;

    // MinorCPU entrega el comando reservado; ya no puede recibir Stall.
    virtual void dispatch(
        const GrantToken &token, const VectorCommand &command) = 0;
};

/**
 * Contrato de retorno que implementará el receptor de MinorCPU.
 * CpuVectorInterface comunica por él la aceptación y la finalización.
 */
class CpuCompletionEndpoint
{
  public:
    virtual ~CpuCompletionEndpoint() = default;

    // Admisión confirma la inserción en la FIFO; aún no ha terminado.
    virtual void accepted(CommandKey command) = 0;
    // AraSequencer informa del cierre; Minor procesa el resultado o el fallo.
    virtual void completed(const VectorCompletion &completion) = 0;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_INTERFACE_CPU_VECTOR_INTERFACE_HH__
