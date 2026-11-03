// Copyright (c) 2026 Félix Garcia Narocki (UCM)
// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/vpu/vlsu/memory_block_planner.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"

namespace gem5::vector_engine
{

MemoryBlock
planMemoryBlock(Addr base, uint32_t element_bytes, uint32_t next_element,
                uint32_t end_element, const MemoryBlockLimits &limits)
{
    panic_if(next_element >= end_element || !element_bytes ||
                 element_bytes > limits.maxBlockBytes ||
                 !limits.boundaries.valid() ||
                 (limits.addressMask != std::numeric_limits<Addr>::max() &&
                  limits.addressMask != Addr{0xffffffff}),
             "Invalid unit-stride block planning request");

    // next_element < 2^32 y element_bytes <= maxBlockBytes < 2^32: el
    // producto cabe en 64 bits. La suma con base se reduce al ancho del
    // objetivo, igual que la dirección efectiva escalar.
    const Addr address =
        (base + Addr{next_element} * element_bytes) & limits.addressMask;

    // Todos los candidatos se comparan en 64 bits. maxBlockBytes acota el
    // mínimo, así que el tamaño resultante cabe en 32 bits.
    const uint64_t remaining_bytes =
        uint64_t{end_element - next_element} * element_bytes;
    const Addr line_bytes = limits.boundaries.lineBytes;
    const Addr page_bytes = limits.boundaries.pageBytes;
    uint64_t block_bytes =
        std::min<uint64_t>({remaining_bytes, limits.maxBlockBytes,
                            line_bytes - address % line_bytes,
                            page_bytes - address % page_bytes});

    // Bytes hasta el final del espacio de direcciones: addressMask - address
    // + 1. Se compara sin sumar uno para no desbordar con address == 0 y
    // máscara de 64 bits; block_bytes >= 1 en este punto.
    if (limits.addressMask - address < block_bytes - 1) {
        block_bytes = limits.addressMask - address + 1;
    }

    MemoryBlock block;
    block.virtualAddress = address;
    const uint64_t element_count = block_bytes / element_bytes;
    if (element_count == 0) {
        // El elemento no cabe entero antes de la frontera más cercana. Se
        // envía solo y el backend lo divide; el bloque siguiente vuelve a
        // agrupar a partir del elemento posterior.
        block.elements = ElementRange{next_element, 1};
        block.bytes = element_bytes;
        block.splitElement = true;
    } else {
        block.elements =
            ElementRange{next_element, static_cast<uint32_t>(element_count)};
        block.bytes = static_cast<uint32_t>(element_count * element_bytes);
    }
    panic_if(block.elements.end() > end_element,
             "Unit-stride block exceeds the active element range");
    return block;
}

} // namespace gem5::vector_engine
