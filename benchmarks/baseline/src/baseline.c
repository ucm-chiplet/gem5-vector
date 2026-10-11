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

#include <stddef.h>
#include <stdint.h>

/* Linux SE startup: use the supplied stack, set gp, then exit(main()). */
__asm__ (
    ".section .text\n"
    ".global _start\n"
    ".type _start, @function\n"
    "_start:\n"
    ".option push\n"
    ".option norelax\n"
    "la gp, __global_pointer$\n"
    ".option pop\n"
    "call main\n"
    "li a7, 93\n"
    "ecall\n"
    "1: j 1b\n"
    ".size _start, .-_start\n"
);

/* The only runtime service besides exit is Linux write(1, data, size). */
static void
writeText(const char *data, size_t size)
{
    while (size != 0) {
        register long a0 __asm__("a0") = 1;
        register const char *a1 __asm__("a1") = data;
        register size_t a2 __asm__("a2") = size;
        register long a7 __asm__("a7") = 64;
        __asm__ volatile ("ecall"
                          : "+r" (a0)
                          : "r" (a1), "r" (a2), "r" (a7)
                          : "memory");
        if (a0 <= 0)
            return;
        data += a0;
        size -= (size_t)a0;
    }
}

static void
printText(const char *text)
{
    size_t size = 0;
    while (text[size] != '\0')
        ++size;
    writeText(text, size);
}

static void
printNumber(size_t value)
{
    char buffer[20];
    size_t pos = sizeof(buffer);
    do {
        buffer[--pos] = '0' + value % 10;
        value /= 10;
    } while (value != 0);
    writeText(buffer + pos, sizeof(buffer) - pos);
}

#define MAX_ELEMENTS 65

static uint32_t inputA[MAX_ELEMENTS];
static uint32_t inputB[MAX_ELEMENTS];
static volatile uint32_t output[MAX_ELEMENTS];

static void
printCase(size_t case_id, size_t count, uint32_t scalar)
{
    printText("[CASO ");
    printNumber(case_id);
    printText(" elementos=");
    printNumber(count);
    printText(" escalar=");
    if (scalar & UINT32_C(0x80000000)) {
        printText("-");
        printNumber((uint32_t)(0 - scalar));
    } else {
        printNumber(scalar);
    }
    printText("] ");
}

/* Read each vector store with scalar loads before starting the next chunk. */
static int
runCase(size_t case_id, size_t count, uint32_t scalar)
{
    printCase(case_id, count, scalar);
    printText("INICIO: probar resultado[i] = A[i] + B[i] + escalar, "
              "modulo 2^32; SEW=32, LMUL=1, sin mascara.\n");
    printCase(case_id, count, scalar);
    printText("PREPARAR: entradas con wraparound y salida con "
              "centinela 0xdeadbeef para detectar escrituras "
              "fuera de rango.\n");
    for (size_t i = 0; i < MAX_ELEMENTS; ++i) {
        inputA[i] = UINT32_C(0xfffffff0) + (uint32_t)i * 3;
        inputB[i] = (uint32_t)i * 7 + 11;
        output[i] = UINT32_C(0xdeadbeef);
    }

    if (count == 0) {
        printCase(case_id, count, scalar);
        printText("VACIO: no ejecutar RVV; comprobar que toda la salida "
                  "conserva el centinela.\n");
    }

    size_t offset = 0;
    while (offset < count) {
        size_t remaining = count - offset;
        size_t vl;
        const uint32_t *a = inputA + offset;
        const uint32_t *b = inputB + offset;
        volatile uint32_t *dst = output + offset;

        printCase(case_id, count, scalar);
        printText("BLOQUE: inicio=");
        printNumber(offset);
        printText(" restantes=");
        printNumber(remaining);
        printText("; vsetvli configura vl, dos vle32.v cargan A/B, "
                  "vadd.vv suma A+B, vadd.vx suma el escalar y "
                  "vse32.v guarda; despues verificar con loads escalares "
                  "inmediatos, sin barrera ni logs intermedios.\n");

        __asm__ volatile (
            ".option push\n\t"
            ".option arch, +v\n\t"
            "vsetvli %[vl], %[remaining], e32, m1, ta, ma\n\t"
            "vle32.v v8, (%[a])\n\t"
            "vle32.v v9, (%[b])\n\t"
            "vadd.vv v10, v8, v9\n\t"
            "vadd.vx v10, v10, %[scalar]\n\t"
            "vse32.v v10, (%[dst])\n\t"
            ".option pop\n\t"
            : [vl] "=&r" (vl)
            : [remaining] "r" (remaining), [a] "r" (a), [b] "r" (b),
              [scalar] "r" (scalar), [dst] "r" (dst)
            /* C is compiled for rv64gc and cannot allocate vector regs. */
            : "memory"
        );

        if (vl == 0 || vl > remaining) {
            printCase(case_id, count, scalar);
            printText("FAIL: invalid vl=");
            printNumber(vl);
            printText(", remaining=");
            printNumber(remaining);
            printText("\n");
            return 1;
        }

        for (size_t i = offset; i < offset + vl; ++i) {
            uint32_t expected = inputA[i] + inputB[i] + scalar;
            uint32_t actual = output[i];
            if (actual != expected) {
                printCase(case_id, count, scalar);
                printText("FAIL: count=");
                printNumber(count);
                printText(", index=");
                printNumber(i);
                printText(", got=");
                printNumber(actual);
                printText(", expected=");
                printNumber(expected);
                printText("\n");
                return 1;
            }
        }
        /* Log only after scalar verification: write() may serialize work. */
        printCase(case_id, count, scalar);
        printText("PASS BLOQUE: vl=");
        printNumber(vl);
        printText("; elementos [");
        printNumber(offset);
        printText(", ");
        printNumber(offset + vl);
        printText(") correctos tras el store vectorial.\n");
        offset += vl;
    }

    printCase(case_id, count, scalar);
    printText("LIMITES: comprobar centinelas en elementos [");
    printNumber(count);
    printText(", ");
    printNumber(MAX_ELEMENTS);
    printText("); detectar escrituras mas alla del tamano solicitado.\n");
    for (size_t i = count; i < MAX_ELEMENTS; ++i) {
        if (output[i] != UINT32_C(0xdeadbeef)) {
            printCase(case_id, count, scalar);
            printText("FAIL: write past count=");
            printNumber(count);
            printText(", index=");
            printNumber(i);
            printText("\n");
            return 1;
        }
    }
    printCase(case_id, count, scalar);
    if (count == 0) {
        printText("PASS CASO: rango vacio; todos los centinelas "
                  "conservados sin ejecutar RVV.\n");
    } else {
        printText("PASS CASO: suma, lectura escalar tras store y limites "
                  "correctos.\n");
    }
    return 0;
}

int
main(void)
{
    const size_t sizes[] = {0, 1, 7, 8, 9, 17, MAX_ELEMENTS};
    const uint32_t scalars[] = {0, 5, UINT32_C(0xfffffffd)};
    size_t cases = 0;

    printText("[SUITE baseline] INICIO: 21 casos, tamanos "
              "0/1/7/8/9/17/65 y escalares 0/5/-3; probar rango vacio, "
              "bloques completos, colas parciales y wraparound.\n");
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        for (size_t k = 0; k < sizeof(scalars) / sizeof(scalars[0]); ++k) {
            if (runCase(cases + 1, sizes[s], scalars[k]) != 0)
                return 1;
            ++cases;
        }
    }
    printText("[SUITE baseline] PASS: ");
    printNumber(cases);
    printText(" baseline cases\n");
    return 0;
}
