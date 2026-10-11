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

/* A small example: output[i] = inputA[i] + inputB[i] + 5. */
#define ELEMENTS 4

static const uint32_t inputA[ELEMENTS] = {1, 2, 3, 4};
static const uint32_t inputB[ELEMENTS] = {10, 20, 30, 40};
/* Initialized data also avoids an empty ELF LOAD segment in gem5. */
static volatile uint32_t output[ELEMENTS] = {99, 99, 99, 99};

int
main(void)
{
    const uint32_t scalar = 5;
    size_t offset = 0;

    printText("[baseline-simple] A={1,2,3,4}, B={10,20,30,40}, scalar=5\n");
    while (offset < ELEMENTS) {
        size_t remaining = ELEMENTS - offset;
        size_t vl;
        const uint32_t *a = inputA + offset;
        const uint32_t *b = inputB + offset;
        volatile uint32_t *dst = output + offset;

        /*
         * MinorCPU executes vsetvli: SEW=32, LMUL=1, no mask.
         * Loads, additions and store are offloaded to the VPU.
         * v8/v9 hold the inputs; v10 holds the intermediate/result.
         * The native mode executes the same instructions in MinorCPU.
         */
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
            /* C uses rv64gc, so it cannot allocate vector registers. */
            : "memory"
        );

        if (vl == 0 || vl > remaining) {
            printText("[baseline-simple] FAIL: invalid vl\n");
            return 1;
        }
        /* Scalar reads immediately check the vector store, before logs. */
        for (size_t i = offset; i < offset + vl; ++i) {
            if (output[i] != inputA[i] + inputB[i] + scalar) {
                printText("[baseline-simple] FAIL: incorrect result\n");
                return 1;
            }
        }
        offset += vl;
    }
    printText("[baseline-simple] PASS: resultado={16,27,38,49}\n");
    return 0;
}
