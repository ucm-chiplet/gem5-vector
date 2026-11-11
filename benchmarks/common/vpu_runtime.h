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

/*
 * Freestanding runtime for VPU benchmarks in gem5 SE mode, without libc.
 *
 * A libc built with V would add RVV instruction types outside the kernel,
 * so benchmarks provide their own startup and use only the Linux write and
 * exit system calls. Include this header from exactly one source file: it
 * defines _start and static helpers.
 *
 * Contract with benchmarks/common/vpu_bench.py: the program prints
 * "[name] checksum: ..." and "[name] PASS: ...", returns 0 from main on
 * success and calls vpuRoiBegin()/vpuRoiEnd() around its kernel.
 */

#ifndef VPU_RUNTIME_H
#define VPU_RUNTIME_H

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

/* Linux write(1, data, size), retried until all bytes are written. */
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

/* Prints a signed decimal value without libc. */
static void
printInt(long value)
{
    char digits[24];
    size_t pos = sizeof(digits);
    unsigned long magnitude;

    if (value < 0)
        magnitude = 0UL - (unsigned long)value;
    else
        magnitude = (unsigned long)value;
    do {
        digits[--pos] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0)
        digits[--pos] = '-';
    writeText(&digits[pos], sizeof(digits) - pos);
}

/*
 * gem5 RISC-V m5ops: opcode 0x7b (custom-3) with the function in bits
 * 31:25 (util/m5/src/abi/riscv/m5op.S). a0 (delay) and a1 (period) are 0,
 * so the action is immediate and not repeated. 0x40 resets and 0x41 dumps
 * the statistics (include/gem5/asm/generic/m5ops.h). They are emitted
 * inline to avoid linking libm5.
 *
 * Minor retires in order and a vector instruction retires only after the
 * VPU completes it, so the dump happens after all kernel work.
 */
static void
vpuRoiBegin(void)
{
    register long a0 __asm__("a0") = 0;
    register long a1 __asm__("a1") = 0;
    __asm__ volatile (".long 0x8000007b" /* 0x7b | (0x40 << 25) */
                      : "+r" (a0) : "r" (a1) : "memory");
}

static void
vpuRoiEnd(void)
{
    register long a0 __asm__("a0") = 0;
    register long a1 __asm__("a1") = 0;
    __asm__ volatile (".long 0x8200007b" /* 0x7b | (0x41 << 25) */
                      : "+r" (a0) : "r" (a1) : "memory");
}

#endif /* VPU_RUNTIME_H */
