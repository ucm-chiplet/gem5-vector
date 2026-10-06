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

namespace gem5::vector_engine
{

CpuVectorInterface::~CpuVectorInterface() = default;

GrantResult
CpuVectorInterface::requestGrant(const VectorCommand &command)
{
    panic_if(!command.command.valid(),
             "CpuVectorInterface received an invalid CommandKey");
    return vpuEndpoint.requestGrant(command);
}

void
CpuVectorInterface::dispatch(const GrantToken &token,
                             const VectorCommand &command)
{
    vpuEndpoint.dispatch(token, command);
}

void
CpuVectorInterface::accepted(CommandKey command)
{
    completionEndpoint.accepted(command);
}

void
CpuVectorInterface::completed(const VectorCompletion &completion)
{
    completionEndpoint.completed(completion);
}

CommandKey
CpuVectorInterface::allocateCommandKey(ContextID contextId)
{
    fatal_if(contextId == InvalidContextID,
             "CpuVectorInterface requires a valid context ID");

    auto &next_id = nextCommandIds[contextId];
    panic_if(next_id == CommandKey::InvalidCommandId,
             "CpuVectorInterface command IDs exhausted");

    return CommandKey{next_id++,
                      contextId}; // Increment next_id for the context
}

} // namespace gem5::vector_engine
