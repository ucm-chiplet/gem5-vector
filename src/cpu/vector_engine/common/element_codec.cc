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

#include "cpu/vector_engine/common/element_codec.hh"

#include <limits>

#include "base/logging.hh"

namespace gem5::vector_engine
{

bool
ElementCodec::validWidth(uint16_t bits)
{
    return bits == 8 || bits == 16 || bits == 32 || bits == 64;
}

uint32_t
ElementCodec::bytes(uint16_t bits)
{
    panic_if(!validWidth(bits), "Invalid element representation width");
    return bits / 8;
}

uint64_t
ElementCodec::mask(uint16_t bits)
{
    panic_if(!validWidth(bits), "Invalid element mask width");
    // Partir de 64 unos permite obtener también la máscara de 64 bits:
    // en ese caso el desplazamiento es cero.
    return std::numeric_limits<uint64_t>::max() >> (64 - bits);
}

uint64_t
ElementCodec::read(const ByteBuffer &data, std::size_t element, uint16_t bits,
                   ByteOrder byte_order)
{
    const auto size = bytes(bits);
    panic_if(
        (byte_order != ByteOrder::little && byte_order != ByteOrder::big) ||
            data.size() < size || element > (data.size() - size) / size,
        "Invalid element byte order or buffer range");
    // element cuenta elementos dentro del buffer; offset cuenta bytes.
    const auto offset = element * size;
    uint64_t value = 0;
    for (unsigned byte = 0; byte < size; ++byte) {
        // El orden del objetivo decide qué byte contiene los bits bajos.
        unsigned shift = 8 * byte;
        if (byte_order == ByteOrder::big) {
            shift = 8 * (size - 1 - byte);
        }
        value |= uint64_t{data[offset + byte]} << shift;
    }
    return value;
}

void
ElementCodec::write(ByteBuffer &data, std::size_t element, uint16_t bits,
                    uint64_t value, ByteOrder byte_order)
{
    const auto size = bytes(bits);
    panic_if(
        (byte_order != ByteOrder::little && byte_order != ByteOrder::big) ||
            data.size() < size || element > (data.size() - size) / size,
        "Invalid element byte order or buffer range");
    const auto offset = element * size;
    for (unsigned byte = 0; byte < size; ++byte) {
        // Usar el mismo orden que read permite recuperar el valor escrito.
        unsigned shift = 8 * byte;
        if (byte_order == ByteOrder::big) {
            shift = 8 * (size - 1 - byte);
        }
        data[offset + byte] = static_cast<uint8_t>(value >> shift);
    }
}

uint64_t
ElementCodec::normalizeScalar(uint64_t value, unsigned xlen,
                              uint16_t element_bits)
{
    panic_if(xlen != 32 && xlen != 64, "Invalid scalar register width");
    // Primero conservar sólo los bits que existen en el registro CPU.
    value &= mask(xlen);
    // Si el elemento es mayor, repetir el bit de signo en los bits nuevos.
    if (xlen < element_bits && (value & (uint64_t{1} << (xlen - 1)))) {
        value |= ~mask(xlen);
    }
    // El comando guarda únicamente los bits que usará cada elemento.
    return value & mask(element_bits);
}

} // namespace gem5::vector_engine
