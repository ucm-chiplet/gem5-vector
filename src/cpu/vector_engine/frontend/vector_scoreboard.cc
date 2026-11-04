// Copyright (c) 2026 Félix Garcia Narocki (UCM)
// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/frontend/vector_scoreboard.hh"

#include <variant>

#include "base/logging.hh"
#include "cpu/vector_engine/common/operation_traits.hh"

namespace gem5::vector_engine
{

RegisterFootprint
VectorScoreboard::footprint(const VectorCommand &command)
{
    RegisterFootprint result;

    for (const auto &operand : describeOperands(command)) {
        const auto *reg = std::get_if<VectorRegRef>(&operand.value);
        if (!reg) {
            continue;
        }
        panic_if(!reg->valid(), "Scoreboard received an invalid group");

        // regCount es el contenedor: en LMUL fraccional vale uno y reserva
        // el registro completo, aunque la operación use sólo una parte.
        RegisterFootprint::Mask group;
        for (uint8_t i = 0; i < reg->regCount; ++i) {
            group.set(reg->firstReg + i);
        }

        if (operand.access == OperandAccess::Read ||
            operand.access == OperandAccess::ReadWrite) {
            result.reads |= group;
        }
        if (operand.access == OperandAccess::Write ||
            operand.access == OperandAccess::ReadWrite) {
            result.writes |= group;
        }
    }

    return result;
}

HazardSet
VectorScoreboard::hazards(const RegisterFootprint &footprint) const
{
    HazardSet result;

    for (const auto &[command, active] : reservations) {
        result.raw = result.raw || (footprint.reads & active.writes).any();
        result.war = result.war || (footprint.writes & active.reads).any();
        result.waw = result.waw || (footprint.writes & active.writes).any();
    }

    return result;
}

void
VectorScoreboard::reserve(CommandKey command,
                          const RegisterFootprint &footprint)
{
    panic_if(!command.valid(), "Scoreboard reservation without identity");

    const auto inserted = reservations.emplace(command, footprint);
    panic_if(!inserted.second, "Duplicate scoreboard reservation");
}

void
VectorScoreboard::release(CommandKey command)
{
    const auto erased = reservations.erase(command);
    panic_if(erased != 1, "Releasing an unknown scoreboard reservation");
}

} // namespace gem5::vector_engine
