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

enum class GrantStatus : uint8_t
{
    Granted,
    Stall,
    Rejected,
};

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

/** Opaque, single-use reservation issued for one specific command. */
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

/** Result of admission without modifying the command FIFO. */
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

/** Non-owning contract implemented by the VPU admission frontend. */
class VpuCommandEndpoint
{
  public:
    virtual ~VpuCommandEndpoint() = default;

    virtual GrantResult requestGrant(const VectorCommand &command) = 0;

    virtual void dispatch(
        const GrantToken &token, const VectorCommand &command) = 0;
};

/** Non-owning contract implemented by the CPU completion receiver. */
class CpuCompletionEndpoint
{
  public:
    virtual ~CpuCompletionEndpoint() = default;

    virtual void accepted(CommandKey command) = 0;
    virtual void completed(const VectorCompletion &completion) = 0;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_INTERFACE_CPU_VECTOR_INTERFACE_HH__
