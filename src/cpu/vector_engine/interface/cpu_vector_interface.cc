/*
 * Copyright (c) 2026 Javier Bautista Luis (UCM)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/vector_engine/interface/cpu_vector_interface.hh"

#include "base/logging.hh"
#include "cpu/vector_engine/common/vector_support.hh"

namespace gem5::vector_engine
{

CpuVectorInterface::CpuVectorInterface(CpuCompletionEndpoint &cpu_endpoint,
                                       VpuCommandEndpoint &vpu_endpoint,
                                       statistics::Group &stats_parent)
    : stats(&stats_parent, "vectorInterface", "dispatched vector commands"),
      keysAllocated(&stats, "keysAllocated", statistics::units::Count::get(),
                    "Command identities allocated, including ungranted work"),
      grants(&stats, "grants", statistics::units::Count::get(),
             "Command reservations granted by the VPU"),
      acceptedNotifications(&stats, "acceptedNotifications",
                            statistics::units::Count::get(),
                            "FIFO acceptance callbacks received from VPU"),
      completionEndpoint(cpu_endpoint),
      vpuEndpoint(vpu_endpoint)
{}

CpuVectorInterface::~CpuVectorInterface()
{
    panic_if(!commands.empty(), "Destroying CPU interface with live commands");
}

GrantResult
CpuVectorInterface::requestGrant(const VectorCommand &command)
{
    // Un comando incorrecto o ya reservado no debe consumir otra entrada.
    if (const auto reason = VectorSupport::checkStructure(command)) {
        ++stats.rejected;
        return GrantResult::rejected(*reason);
    }
    if (commands.count(command.command)) {
        ++stats.rejected;
        return GrantResult::rejected(RejectionReason::DuplicateCommand);
    }
    const auto result = vpuEndpoint.requestGrant(command);
    panic_if(!result.consistent(), "Inconsistent grant response");
    switch (result.status) {
        case GrantStatus::Granted:
            panic_if(result.token->command != command.command,
                     "Grant response belongs to another command");
            // Conservar lo reservado para comprobar luego el envío y cierre.
            commands.emplace(command.command,
                             CommandState{command, *result.token});
            ++grants;
            break;
        case GrantStatus::Stall:
            ++stats.retries;
            break;
        case GrantStatus::Rejected:
            ++stats.rejected;
            break;
    }
    return result;
}

void
CpuVectorInterface::dispatch(const GrantToken &token,
                             const VectorCommand &command)
{
    const auto found = commands.find(command.command);
    panic_if(found == commands.end() ||
                 found->second.phase != Phase::Reserved ||
                 found->second.token != token ||
                 found->second.descriptor != command,
             "Dispatch does not match the reserved immutable command");
    // Marcar el envío antes de llamar a la VPU: ésta puede responder dentro
    // de dispatch, sin esperar a que la llamada termine.
    found->second.phase = Phase::Dispatched;
    uint64_t count = 0;
    if (command.config.vl > command.config.vstart) {
        count = command.config.vl - command.config.vstart;
    }
    stats.accept(count, uint64_t{count} * VectorSupport::elementBytes(
                                              command.config.sewBits));
    vpuEndpoint.dispatch(token, command);
}

void
CpuVectorInterface::accepted(CommandKey command)
{
    const auto found = commands.find(command);
    panic_if(found == commands.end() ||
                 found->second.phase != Phase::Dispatched,
             "Unknown or duplicate CPU acceptance");
    found->second.phase = Phase::Accepted;
    ++acceptedNotifications;
    completionEndpoint.accepted(command);
}

void
CpuVectorInterface::completed(const VectorCompletion &completion)
{
    const auto found = commands.find(completion.command);
    panic_if(found == commands.end() || found->second.phase != Phase::Accepted,
             "Unknown, premature or duplicate CPU completion");
    const auto &command = found->second.descriptor;
    // Sólo aceptar efectos que el comando original podía producir.
    panic_if(!validFpFlags(completion.fpFlags) ||
                 (!command.fpContext && completion.fpFlags) ||
                 completion.scalarResult,
             "Unexpected CPU completion effects");
    if (completion.status == CompletionStatus::Success) {
        panic_if(completion.fault || completion.finalVstart != 0,
                 "Invalid successful completion");
    } else {
        const auto *memory = std::get_if<MemoryCommand>(&command.payload);
        panic_if(
            completion.status != CompletionStatus::MemoryFault || !memory ||
                !completion.fault || completion.fault->fault == NoFault ||
                !completion.fault->address ||
                !completion.fault->elementIndex ||
                completion.finalVstart != *completion.fault->elementIndex ||
                completion.finalVstart < command.config.vstart ||
                completion.finalVstart >= command.config.vl,
            "Invalid memory fault completion");
    }
    // La entrada sigue viva mientras la CPU recibe la notificación final.
    found->second.phase = Phase::Completed;
    stats.complete(completion.status != CompletionStatus::Success);
    completionEndpoint.completed(completion);
    commands.erase(completion.command);
}

CommandKey
CpuVectorInterface::allocateCommandKey(ContextID contextId)
{
    fatal_if(contextId == InvalidContextID,
             "CpuVectorInterface requires a valid context ID");

    auto &next_id = nextCommandIds[contextId];
    panic_if(next_id == CommandKey::InvalidCommandId,
             "CpuVectorInterface command IDs exhausted");

    ++keysAllocated;
    return CommandKey{next_id++,
                      contextId}; // Increment next_id for the context
}

} // namespace gem5::vector_engine
