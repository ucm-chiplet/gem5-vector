#ifndef __CPU_VECTOR_ENGINE_COMMON_COMMAND_KEY_HH__
#define __CPU_VECTOR_ENGINE_COMMON_COMMAND_KEY_HH__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

#include "base/types.hh"

namespace gem5::vector_engine
{

/**
 * Identity of a vector command.
 *
 * A command identifier is only unique within its owning context.  The full
 * identity must therefore be propagated whenever work derived from a command
 * crosses a module boundary.
 */
struct CommandKey
{
    static constexpr uint64_t InvalidCommandId =
        std::numeric_limits<uint64_t>::max();

    uint64_t commandId = InvalidCommandId;
    ContextID contextId = InvalidContextID;

    constexpr bool
    valid() const
    {
        return commandId != InvalidCommandId &&
               contextId != InvalidContextID;
    }
};

constexpr bool
operator==(const CommandKey &lhs, const CommandKey &rhs)
{
    return lhs.commandId == rhs.commandId && lhs.contextId == rhs.contextId;
}

constexpr bool
operator!=(const CommandKey &lhs, const CommandKey &rhs)
{
    return !(lhs == rhs);
}

constexpr bool
operator<(const CommandKey &lhs, const CommandKey &rhs)
{
    return lhs.contextId < rhs.contextId ||
           (lhs.contextId == rhs.contextId &&
            lhs.commandId < rhs.commandId);
}

/** Hash function for command-indexed protocol state. */
struct CommandKeyHash
{
    std::size_t
    operator()(const CommandKey &command) const
    {
        const auto context_hash =
            std::hash<ContextID>{}(command.contextId);
        const auto command_hash =
            std::hash<uint64_t>{}(command.commandId);

        return context_hash ^
               (command_hash + 0x9e3779b9U +
                (context_hash << 6) + (context_hash >> 2));
    }
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_COMMAND_KEY_HH__
