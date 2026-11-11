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
 * Micro-kernel with independent vector arithmetic, to observe commands
 * executing together in the VPU (criterion 1 of hito 2) and the effect of
 * ALUs and VRF ports (criterion 5).
 *
 * Each strip of vl elements (SEW=32, LMUL=m1) loads its sources, then runs
 * four consecutive arithmetic instructions with no dependency between
 * them, then stores the four results:
 *
 *   integer (default)        mixed (-DMIXED=1)
 *   o0 = a + b   (vadd.vv)   o0 = a + b   (vadd.vv)
 *   o1 = c * d   (vmul.vv)   o1 = e + f   (vfadd.vv, FP32)
 *   o2 = min(a,d)(vmin.vv)   o2 = min(a,d)(vmin.vv)
 *   o3 = b + c   (vadd.vv)   o3 = e * f   (vfmul.vv, FP32)
 *
 * The kernel is written in inline assembly with fixed vector registers so
 * that the compiler cannot reuse a source register as a destination: a
 * reuse would add a WAR hazard and serialize the instructions, as happens
 * with Pathfinder. Hito 2 keeps vector memory as a barrier, so loads and
 * stores do not overlap; only the four arithmetic instructions can.
 *
 * The data are generated and checked with scalar code (-fno-vectorize).
 * Arrays are aligned to 64 bytes so that no 32-byte vector access crosses
 * a memory line. RVV 1.0 (edition 20240411): Vector Single-Width Integer
 * Add, Multiply, Min/Max and Floating-Point Add/Multiply Instructions;
 * FP32 uses the default rounding mode (RNE), as the scalar reference.
 */

#include "vpu_runtime.h"

#ifndef ELEMENTS
#define ELEMENTS 1024
#endif

#ifndef MIXED
#define MIXED 0
#endif

#define ALIGNED __attribute__((aligned(64)))

static int32_t a[ELEMENTS] ALIGNED;
static int32_t b[ELEMENTS] ALIGNED;
static int32_t c[ELEMENTS] ALIGNED;
static int32_t d[ELEMENTS] ALIGNED;
static float e[ELEMENTS] ALIGNED;
static float f[ELEMENTS] ALIGNED;
/* Four outputs; in the mixed variant o1 and o3 hold FP32 bit patterns. */
static uint32_t o0[ELEMENTS] ALIGNED;
static uint32_t o1[ELEMENTS] ALIGNED;
static uint32_t o2[ELEMENTS] ALIGNED;
static uint32_t o3[ELEMENTS] ALIGNED;

/* Deterministic LCG; the low bits are discarded. */
static uint32_t
nextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

/*
 * Integers in [-1000, 999] and floats that are multiples of 1/8 in
 * [-125, 125): exactly representable, so the inputs carry no rounding.
 */
static void
initData(void)
{
    uint32_t state = 12345u;
    for (size_t i = 0; i < ELEMENTS; ++i) {
        a[i] = (int32_t)(nextRandom(&state) % 2000u) - 1000;
        b[i] = (int32_t)(nextRandom(&state) % 2000u) - 1000;
        c[i] = (int32_t)(nextRandom(&state) % 2000u) - 1000;
        d[i] = (int32_t)(nextRandom(&state) % 2000u) - 1000;
        e[i] = (float)((int32_t)(nextRandom(&state) % 2000u) - 1000) / 8.0f;
        f[i] = (float)((int32_t)(nextRandom(&state) % 2000u) - 1000) / 8.0f;
    }
}

/*
 * Vector kernel. One asm block per strip keeps the vsetvli and every
 * vector instruction together, with explicit registers and clobbers:
 * v1-v6 hold sources and v8-v11 results.
 */
static __attribute__((noinline)) void
runKernel(void)
{
    size_t vl;
    for (size_t n = 0; n < ELEMENTS; n += vl) {
#if MIXED
        __asm__ volatile (
            "vsetvli %[vl], %[avl], e32, m1, ta, ma\n"
            "vle32.v v1, (%[a])\n"
            "vle32.v v2, (%[b])\n"
            "vle32.v v4, (%[d])\n"
            "vle32.v v5, (%[e])\n"
            "vle32.v v6, (%[f])\n"
            "vadd.vv v8, v1, v2\n"
            "vfadd.vv v9, v5, v6\n"
            "vmin.vv v10, v1, v4\n"
            "vfmul.vv v11, v5, v6\n"
            "vse32.v v8, (%[o0])\n"
            "vse32.v v9, (%[o1])\n"
            "vse32.v v10, (%[o2])\n"
            "vse32.v v11, (%[o3])\n"
            : [vl] "=&r" (vl)
            : [avl] "r" (ELEMENTS - n), [a] "r" (&a[n]), [b] "r" (&b[n]),
              [d] "r" (&d[n]), [e] "r" (&e[n]), [f] "r" (&f[n]),
              [o0] "r" (&o0[n]), [o1] "r" (&o1[n]), [o2] "r" (&o2[n]),
              [o3] "r" (&o3[n])
            : "v1", "v2", "v4", "v5", "v6", "v8", "v9", "v10", "v11",
              "memory");
#else
        __asm__ volatile (
            "vsetvli %[vl], %[avl], e32, m1, ta, ma\n"
            "vle32.v v1, (%[a])\n"
            "vle32.v v2, (%[b])\n"
            "vle32.v v3, (%[c])\n"
            "vle32.v v4, (%[d])\n"
            "vadd.vv v8, v1, v2\n"
            "vmul.vv v9, v3, v4\n"
            "vmin.vv v10, v1, v4\n"
            "vadd.vv v11, v2, v3\n"
            "vse32.v v8, (%[o0])\n"
            "vse32.v v9, (%[o1])\n"
            "vse32.v v10, (%[o2])\n"
            "vse32.v v11, (%[o3])\n"
            : [vl] "=&r" (vl)
            : [avl] "r" (ELEMENTS - n), [a] "r" (&a[n]), [b] "r" (&b[n]),
              [c] "r" (&c[n]), [d] "r" (&d[n]), [o0] "r" (&o0[n]),
              [o1] "r" (&o1[n]), [o2] "r" (&o2[n]), [o3] "r" (&o3[n])
            : "v1", "v2", "v3", "v4", "v8", "v9", "v10", "v11", "memory");
#endif
    }
}

#if MIXED
/* Raw bits of a float, without type punning through pointers. */
static uint32_t
floatBits(float value)
{
    union
    {
        float f;
        uint32_t u;
    } bits;
    bits.f = value;
    return bits.u;
}
#endif

/* Prints the first mismatch of one output and returns 1, or returns 0. */
static int
checkOutput(const char *name, size_t i, uint32_t got, uint32_t expected)
{
    if (got == expected)
        return 0;
    printText("[concurrency] FAIL: ");
    printText(name);
    printText("[");
    printInt((long)i);
    printText("]=");
    printInt((long)got);
    printText(", expected=");
    printInt((long)expected);
    printText("\n");
    return 1;
}

/*
 * Scalar reference. Integer add and multiply wrap modulo 2^32, as RVV
 * keeps the SEW low bits; vmin compares as signed. FP32 results are
 * compared bit by bit with scalar F instructions in the same rounding mode.
 */
static int
verify(void)
{
    for (size_t i = 0; i < ELEMENTS; ++i) {
        const uint32_t sum_ab = (uint32_t)a[i] + (uint32_t)b[i];
        const uint32_t min_ad = (uint32_t)(a[i] < d[i] ? a[i] : d[i]);
#if MIXED
        const uint32_t out1 = floatBits(e[i] + f[i]);
        const uint32_t out3 = floatBits(e[i] * f[i]);
#else
        const uint32_t out1 = (uint32_t)c[i] * (uint32_t)d[i];
        const uint32_t out3 = (uint32_t)b[i] + (uint32_t)c[i];
#endif
        if (checkOutput("o0", i, o0[i], sum_ab) ||
            checkOutput("o1", i, o1[i], out1) ||
            checkOutput("o2", i, o2[i], min_ad) ||
            checkOutput("o3", i, o3[i], out3)) {
            return 1;
        }
    }
    return 0;
}

/* Fingerprint of all outputs, to compare modes beyond PASS. */
static void
printChecksum(void)
{
    uint64_t sum = 0;
    uint32_t xor_value = 0;
    for (size_t i = 0; i < ELEMENTS; ++i) {
        sum += (uint64_t)o0[i] + o1[i] + o2[i] + o3[i];
        xor_value ^= o0[i] ^ o1[i] ^ o2[i] ^ o3[i];
    }
    printText("[concurrency] checksum: sum=");
    printInt((long)sum);
    printText(" xor=");
    printInt((long)xor_value);
    printText("\n");
}

int
main(void)
{
    initData();
    vpuRoiBegin();
    runKernel();
    vpuRoiEnd();

    printChecksum();
    if (verify() != 0)
        return 1;
    printText("[concurrency] PASS: elements=");
    printInt(ELEMENTS);
    printText(MIXED ? ", mixed\n" : ", integer\n");
    return 0;
}
