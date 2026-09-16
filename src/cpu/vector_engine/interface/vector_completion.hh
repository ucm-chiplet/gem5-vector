#ifndef __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMPLETION_HH__
#define __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMPLETION_HH__

#include <cstdint>
#include <optional>

#include "base/types.hh"
#include "cpu/vector_engine/common/command_key.hh"

namespace gem5::vector_engine
{

enum class CompletionStatus : uint8_t
{
    Success,
    MemoryFault,
    IllegalInstruction,
    InternalError,
    Cancelled,
};

struct ScalarResult
{
    RegIndex destination = 0;
    RegVal value = 0;
};

struct FaultInfo
{
    Fault fault = NoFault;
    std::optional<Addr> address;
    std::optional<uint32_t> elementIndex;
};

/**
 * Terminal event for a command accepted by the VPU.
 *
 * A successful completion carries no fault.  A failing completion carries a
 * valid FaultInfo; memory faults may additionally identify the address and
 * vector element which caused the failure.
 */
struct VectorCompletion
{
    CommandKey command;
    CompletionStatus status = CompletionStatus::InternalError;
    std::optional<ScalarResult> scalarResult;
    uint32_t finalVstart = 0;
    std::optional<FaultInfo> fault;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMPLETION_HH__
