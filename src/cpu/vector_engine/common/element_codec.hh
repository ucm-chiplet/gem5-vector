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
 * Lee y escribe elementos guardados como bytes, respetando el orden de
 * bytes de la CPU simulada. El ordenador que ejecuta gem5 puede usar otro
 * orden. Poder representar un ancho aquí no habilita sus instrucciones.
 */
class ElementCodec
{
  public:
    /** Indica si sabemos leer y escribir elementos de ese número de bits. */
    static bool validWidth(uint16_t bits);
    /** Devuelve cuántos bytes ocupa un elemento de un ancho admitido. */
    static uint32_t bytes(uint16_t bits);
    /** Devuelve un valor con unos en los bits que pertenecen al elemento. */
    static uint64_t mask(uint16_t bits);
    /**
     * Reúne los bytes de un elemento en un valor de 64 bits. element es su
     * posición dentro de data, empezando por cero. El buffer debe contener
     * el elemento completo; aquí no se interpreta su signo ni su formato.
     */
    static uint64_t read(const ByteBuffer &data, std::size_t element,
                         uint16_t bits, ByteOrder byte_order);
    /**
     * Guarda value en la posición element de data. Si sobran bits altos,
     * se descartan. El espacio debe existir y los demás bytes se conservan.
     */
    static void write(ByteBuffer &data, std::size_t element, uint16_t bits,
                      uint64_t value, ByteOrder byte_order);
    /**
     * Adapta un valor escalar al tamaño del elemento vectorial: conserva
     * los bits bajos si sobra anchura y repite el signo si falta. xlen es
     * el ancho del registro CPU (32 o 64 bits). Se hace una vez al crear
     * el comando, para reutilizar el mismo valor si hay que reintentarlo.
     */
    static uint64_t normalizeScalar(uint64_t value, unsigned xlen,
                                    uint16_t element_bits);
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_ELEMENT_CODEC_HH__
