#ifndef __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMPLETION_HH__
#define __CPU_VECTOR_ENGINE_INTERFACE_VECTOR_COMPLETION_HH__

#include <cstdint>
#include <optional>

#include "base/types.hh"
#include "cpu/vector_engine/common/command_key.hh"

namespace gem5::vector_engine
{

/**
 * Resultado que AraSequencer comunica a MinorCPU al cerrar un comando.
 * La versión inicial sólo emite Success o MemoryFault como respuesta normal.
 */
enum class CompletionStatus : uint8_t
{
    Success,
    MemoryFault,
    IllegalInstruction,
    InternalError,
    Cancelled,
};

/**
 * Resultado escalar reservado para futuras instrucciones de la VPU.
 * MinorCPU sería su consumidor; vsetvli se resuelve en CPU y no usa este tipo.
 */
struct ScalarResult
{
    RegIndex destination = 0;
    RegVal value = 0;
};

/**
 * Causa y posición del fallo que memoria comunica a AraVLSU.
 * AraSequencer las propaga en la finalización para que MinorCPU las procese.
 * En MemoryFault son obligatorios fault distinto de NoFault, address e índice.
 */
struct FaultInfo
{
    Fault fault = NoFault;
    std::optional<Addr> address;
    std::optional<uint32_t> elementIndex;
};

/**
 * AraSequencer la crea al terminar un comando aceptado por la VPU.
 * CpuVectorInterface la devuelve a MinorCPU, que retira o trata la excepción.
 * Success no lleva fault ni resultado escalar y deja finalVstart en cero.
 * MemoryFault exige causa, dirección virtual e índice del elemento;
 * finalVstart debe coincidir con ese índice y scalarResult queda ausente.
 * Los demás estados no son finalizaciones normales de la versión inicial.
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
