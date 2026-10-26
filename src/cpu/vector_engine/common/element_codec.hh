/*
 * Copyright (c) 2026 Félix Garcia Narocki (UCM)
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

#ifndef __CPU_VECTOR_ENGINE_COMMON_ELEMENT_CODEC_HH__
#define __CPU_VECTOR_ENGINE_COMMON_ELEMENT_CODEC_HH__

#include <cstddef>
#include <cstdint>

#include "cpu/vector_engine/common/vector_types.hh"
#include "enums/ByteOrder.hh"

namespace gem5::vector_engine
{

/**
 * Convierte entre bytes y patrones de bits, sin consultar el soporte activo.
 * Las unidades validan la operación; este adaptador sólo interpreta datos.
 * No modifica el tamaño de los buffers ni usa el endianness del host.
 */
class ElementCodec
{
  public:
    /** Indica si el contenedor puede representar ese ancho, en bits. */
    static bool validWidth(uint16_t bits);
    /** Bytes por elemento; exige validWidth(bits). */
    static uint32_t bytes(uint16_t bits);
    /** Máscara de bits activos; también admite 64 sin desplazar por 64. */
    static uint64_t mask(uint16_t bits);
    /**
     * Lee el elemento indicado por su índice dentro del buffer, no por su
     * índice arquitectónico. Exige ancho/orden válidos y bytes suficientes.
     * Devuelve sus bits sin interpretar signo ni formato de coma flotante.
     */
    static uint64_t read(const ByteBuffer &data, std::size_t element,
                         uint16_t bits, ByteOrder byte_order);
    /**
     * Escribe los bits bajos del valor en un elemento ya reservado. Conserva
     * los bytes ajenos al elemento y no adquiere propiedad del buffer.
     */
    static void write(ByteBuffer &data, std::size_t element, uint16_t bits,
                      uint64_t value, ByteOrder byte_order);
    /**
     * Normaliza una fuente .vx capturada por CPU: trunca si XLEN >= SEW y
     * extiende el signo si XLEN < SEW. XLEN es 32/64; SEW debe ser válido.
     * El resultado se conserva en el comando durante todos los reintentos.
     */
    static uint64_t normalizeScalar(uint64_t value, unsigned xlen,
                                    uint16_t element_bits);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_ELEMENT_CODEC_HH__
