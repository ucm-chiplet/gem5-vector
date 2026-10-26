#ifndef __CPU_VECTOR_ENGINE_INTERFACE_CPU_VECTOR_INTERFACE_HH__
#define __CPU_VECTOR_ENGINE_INTERFACE_CPU_VECTOR_INTERFACE_HH__

#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>

#include "cpu/vector_engine/common/command_key.hh"
#include "cpu/vector_engine/common/module_stats.hh"
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
    virtual void dispatch(const GrantToken &token,
                          const VectorCommand &command) = 0;
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

/** Adaptador bidireccional entre Execute/MinorCPU y la VPU. */
class CpuVectorInterface : public VpuCommandEndpoint,
                           public CpuCompletionEndpoint
{
  public:
    explicit CpuVectorInterface(CpuCompletionEndpoint &cpu_endpoint,
                                VpuCommandEndpoint &vpu_endpoint,
                                statistics::Group &stats_parent);

    ~CpuVectorInterface() override;

    GrantResult requestGrant(const VectorCommand &command) override;
    void dispatch(const GrantToken &token,
                  const VectorCommand &command) override;
    void accepted(CommandKey command) override;
    void completed(const VectorCompletion &completion) override;

    CommandKey allocateCommandKey(ContextID contextId);

  private:
    /** Pasos que debe seguir cada comando al cruzar la interfaz. */
    enum class Phase
    {
        Reserved,   // Hay espacio reservado, pero el comando no se ha enviado.
        Dispatched, // Enviado; la VPU aún debe confirmar su aceptación.
        Accepted,   // Está en la cola; todavía puede no haber empezado.
        Completed   // La CPU está recibiendo la respuesta final.
    };
    /**
     * Guarda lo que se reservó para comprobar que se envía y termina el mismo
     * comando. Se crea al obtener espacio y se borra tras notificar el final.
     * La retirada de la instrucción CPU se sigue controlando en Minor.
     */
    struct CommandState
    {
        VectorCommand descriptor;
        GrantToken token;
        Phase phase = Phase::Reserved;
    };

    ModuleStats stats;
    statistics::Scalar keysAllocated;
    statistics::Scalar grants;
    statistics::Scalar acceptedNotifications;
    CpuCompletionEndpoint &completionEndpoint;
    VpuCommandEndpoint &vpuEndpoint;
    /** Although we don't currently support SMT, we track
     * command IDs per context using an unordered_map to
     * facilitate future expansion. Each context ID has its
     * own independent counter. */
    std::unordered_map<ContextID, uint64_t> nextCommandIds;
    // Una entrada por crédito concedido; Stall/Rejected no crean contexto.
    std::unordered_map<CommandKey, CommandState, CommandKeyHash> commands;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_INTERFACE_CPU_VECTOR_INTERFACE_HH__
