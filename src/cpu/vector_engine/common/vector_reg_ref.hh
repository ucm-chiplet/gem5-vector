#ifndef __CPU_VECTOR_ENGINE_COMMON_VECTOR_REG_REF_HH__
#define __CPU_VECTOR_ENGINE_COMMON_VECTOR_REG_REF_HH__

#include <cstdint>
#include <limits>

namespace gem5::vector_engine
{

inline constexpr uint8_t NumArchitecturalVectorRegs = 32;

/**
 * Grupo de registros arquitectónicos que MinorCPU incorpora al comando.
 * AraSequencer y las unidades lo conservan; el VRF ubica con él los datos.
 * regCount cuenta registros contenedores, también para LMUL fraccionario.
 */
struct VectorRegRef
{
    uint8_t firstReg = 0;
    uint8_t regCount = 0;

    // Comprobación local de límites; no valida toda la configuración RVV.
    constexpr bool
    valid() const
    {
        return regCount != 0 &&
               firstReg < NumArchitecturalVectorRegs &&
               regCount <= NumArchitecturalVectorRegs - firstReg;
    }

    /** La admisión usa esta regla para comprobar la alineación del grupo. */
    constexpr bool
    naturallyAligned() const
    {
        return valid() && firstReg % regCount == 0;
    }
};

constexpr bool
operator==(const VectorRegRef &lhs, const VectorRegRef &rhs)
{
    return lhs.firstReg == rhs.firstReg && lhs.regCount == rhs.regCount;
}

constexpr bool
operator!=(const VectorRegRef &lhs, const VectorRegRef &rhs)
{
    return !(lhs == rhs);
}

/**
 * Intervalo de bytes [offset, offset + size), relativo al grupo de registros.
 * AraSequencer y las unidades lo usan en tareas y accesos al VRF.
 * No puede estar vacío; el productor comprueba además la capacidad efectiva.
 */
struct ByteRange
{
    uint32_t offset = 0;
    uint32_t size = 0;

    constexpr bool
    valid() const
    {
        return size != 0 &&
               offset <= std::numeric_limits<uint32_t>::max() - size;
    }

    constexpr uint32_t
    end() const
    {
        return offset + size;
    }

    constexpr bool
    fitsWithin(uint32_t byte_count) const
    {
        return valid() && end() <= byte_count;
    }
};

constexpr bool
operator==(const ByteRange &lhs, const ByteRange &rhs)
{
    return lhs.offset == rhs.offset && lhs.size == rhs.size;
}

constexpr bool
operator!=(const ByteRange &lhs, const ByteRange &rhs)
{
    return !(lhs == rhs);
}

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_VECTOR_REG_REF_HH__
