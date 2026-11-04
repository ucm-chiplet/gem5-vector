// Copyright (c) 2026 Félix Garcia Narocki (UCM)
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_FRONTEND_VECTOR_SCOREBOARD_HH__
#define __CPU_VECTOR_ENGINE_FRONTEND_VECTOR_SCOREBOARD_HH__

#include <bitset>
#include <cstddef>
#include <map>

#include "cpu/vector_engine/common/command_key.hh"
#include "cpu/vector_engine/common/vector_reg_ref.hh"
#include "cpu/vector_engine/interface/vector_command.hh"

namespace gem5::vector_engine
{

/** Registros arquitectónicos completos que lee y escribe un comando. */
struct RegisterFootprint
{
    using Mask = std::bitset<NumArchitecturalVectorRegs>;

    Mask reads;
    Mask writes;
};

/**
 * Riesgos de un comando frente a los que ya están activos. No son
 * excluyentes: un mismo comando puede esperar por varias causas a la vez.
 */
struct HazardSet
{
    // El nuevo lee un registro que escribe un comando activo.
    bool raw = false;
    // El nuevo escribe un registro que lee un comando activo.
    bool war = false;
    // El nuevo escribe un registro que escribe un comando activo.
    bool waw = false;

    bool
    any() const
    {
        return raw || war || waw;
    }
};

/**
 * Scoreboard arquitectónico conservador del hito 2 (diseño, sección 6.2).
 *
 * Registra, por comando activo, los registros vectoriales que lee y que
 * escribe. Reserva el grupo efectivo completo; en LMUL fraccional reserva
 * el registro contenedor. Grupos que coinciden sólo en parte también
 * entran en conflicto. Las lecturas concurrentes de un registro se admiten.
 *
 * No asigna registros físicos ni versiones y no conoce disponibilidad por
 * elementos: un consumidor espera a que se libere toda la reserva de su
 * productor, es decir, a que se cierren sus tareas y writebacks.
 *
 * AraSequencer inicia los comandos en orden, de modo que todas las reservas
 * vivas pertenecen a comandos anteriores al que se consulta. Un comando
 * nunca se compara consigo mismo; la unidad aplica las reglas legales de
 * solapamiento entre sus propias fuentes y su destino.
 */
class VectorScoreboard
{
  public:
    /**
     * Calcula los registros de un comando a partir de describeOperands().
     * Los escalares capturados y las direcciones no ocupan registros.
     */
    static RegisterFootprint footprint(const VectorCommand &command);

    /** Riesgos de footprint frente a todas las reservas activas. */
    HazardSet hazards(const RegisterFootprint &footprint) const;

    /** Reserva al iniciar; la clave no puede estar ya reservada. */
    void reserve(CommandKey command, const RegisterFootprint &footprint);

    /** Libera tras cerrar todas las tareas y writebacks del comando. */
    void release(CommandKey command);

    bool
    empty() const
    {
        return reservations.empty();
    }

    std::size_t
    size() const
    {
        return reservations.size();
    }

  private:
    std::map<CommandKey, RegisterFootprint> reservations;
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_FRONTEND_VECTOR_SCOREBOARD_HH__
