// Copyright (c) 2026 Félix Garcia Narocki (UCM)
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_VPU_VLSU_MEMORY_BLOCK_PLANNER_HH__
#define __CPU_VECTOR_ENGINE_VPU_VLSU_MEMORY_BLOCK_PLANNER_HH__

#include <cstdint>

#include "base/types.hh"
#include "cpu/vector_engine/common/memory_types.hh"
#include "cpu/vector_engine/common/unit_task.hh"

namespace gem5::vector_engine
{

/** Límites que recortan cada bloque; ninguno depende de la instrucción. */
struct MemoryBlockLimits
{
    // Capacidad del buffer del bloque activo, en bytes.
    uint32_t maxBlockBytes = 0;
    MemoryBoundaries boundaries;
    // Máscara del ancho de dirección del objetivo: 32 o 64 bits a uno.
    Addr addressMask = 0;
};

/**
 * Siguiente bloque de un recorrido unit-stride. splitElement indica un
 * bloque de un elemento que cruza una línea, una página o el final del
 * espacio de direcciones; el backend lo transporta en varios fragmentos.
 */
struct MemoryBlock
{
    ElementRange elements;
    Addr virtualAddress = 0;
    uint32_t bytes = 0;
    bool splitElement = false;
};

/**
 * Calcula el bloque que empieza en el índice absoluto next_element.
 *
 * La dirección inicial es base + next_element * element_bytes, con el ancho
 * de dirección del objetivo; vstart sólo determina el primer índice, no
 * desplaza la base. El bloque contiene elementos completos y su tamaño es
 * el menor de: bytes restantes hasta end_element, maxBlockBytes y bytes
 * hasta fin de línea, de página o del espacio de direcciones. Nunca lee
 * elementos anteriores a next_element ni posteriores a end_element.
 *
 * Precondiciones (error interno si no se cumplen): next_element <
 * end_element, element_bytes positivo y menor o igual que maxBlockBytes, y
 * fronteras válidas. Es una función pura: no reserva ni emite nada.
 */
MemoryBlock planMemoryBlock(Addr base, uint32_t element_bytes,
                            uint32_t next_element, uint32_t end_element,
                            const MemoryBlockLimits &limits);

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_VPU_VLSU_MEMORY_BLOCK_PLANNER_HH__
