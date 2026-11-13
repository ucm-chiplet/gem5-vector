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
 * Producer-consumer chains between units with different latencies
 * (criterion 2 of hito 2: a consumer does not start with partial
 * results). SEW 32, LMUL m1.
 *
 * Per strip, one asm block runs four RAW chains and one independent
 * control pair:
 *
 *   FPU -> ALU:     vfmul.vv v3, v13, v14   ->  vadd.vv v4, v3, v13
 *   divider -> ALU: vdiv.vv v5, v1, v2      ->  vadd.vv v6, v5, v1
 *   ALU -> slide:   vadd.vv v7, v1, v2      ->  vslide1down.vx v8, v7, x
 *   slide -> ALU:   vslide1down.vx v9, v1, x -> vmin.vv v10, v9, v2
 *   control:        vdiv.vv v11, v1, v2      ;  vadd.vv v12, v1, v2
 *
 * The control pair is independent, so its vadd may start while the
 * vdiv still runs. check_timeline.py uses it to show that the check
 * detects overlap: every RAW consumer must start after its producer
 * completes, and the control vadd must start before its vdiv completes.
 *
 * vadd.vv v4 adds the bits of the FP32 product and of the FP32 operand
 * as integers, which RVV allows: a vector register has no type.
 * Divisors are never zero. ELEMENTS is not a multiple of VLMAX (8 with
 * VLEN 256), so the last strip is partial and vslide1down inserts x at
 * vl - 1 of each strip.
 */

#include "vpu_runtime.h"

#ifndef ELEMENTS
#define ELEMENTS 60
#endif

#define ALIGNED __attribute__((aligned(64)))
#define SLIDE_SCALAR 7
/* Outputs: v4, v6, v8, v10, v11 and v12. */
#define OUTPUTS 6
#define MAX_STRIPS ELEMENTS

static const char *const outputNames[OUTPUTS] = {
    "fmul->add", "div->add", "add->slide", "slide->min", "div", "add"};

static int32_t inA[ELEMENTS] ALIGNED;
static int32_t inB[ELEMENTS] ALIGNED;
static float inFa[ELEMENTS] ALIGNED;
static float inFb[ELEMENTS] ALIGNED;
static int32_t out[OUTPUTS][ELEMENTS] ALIGNED;
static size_t stripLength[MAX_STRIPS];
static size_t strips;

/* Deterministic LCG; the low bits are discarded. */
static uint32_t
nextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static uint32_t
bitsOf(float value)
{
    union
    {
        float f;
        uint32_t u;
    } bits;
    bits.f = value;
    return bits.u;
}

static void
initData(void)
{
    uint32_t state = 777u;
    for (size_t i = 0; i < ELEMENTS; ++i) {
        inA[i] = (int32_t)(nextRandom(&state) % 200001u) - 100000;
        /* Never zero: 1..999 with either sign. */
        int32_t b = (int32_t)(nextRandom(&state) % 999u) + 1;
        if (nextRandom(&state) & 1u)
            b = -b;
        inB[i] = b;
        inFa[i] = (float)((int32_t)(nextRandom(&state) % 2001u) - 1000) /
                  8.0f;
        inFb[i] = (float)((int32_t)(nextRandom(&state) % 2001u) - 1000) /
                  16.0f;
    }
}

/* One strip; always inlined into runKernel(), as check_profile.py
 * requires. */
#define CHAIN_BLOCK()                                                      \
    __asm__ volatile (                                                     \
        "vsetvli %[vl], %[avl], e32, m1, tu, mu\n"                         \
        "vle32.v v1, (%[a])\n"                                             \
        "vle32.v v2, (%[b])\n"                                             \
        "vle32.v v13, (%[fa])\n"                                           \
        "vle32.v v14, (%[fb])\n"                                           \
        "vfmul.vv v3, v13, v14\n"                                          \
        "vadd.vv v4, v3, v13\n"                                            \
        "vdiv.vv v5, v1, v2\n"                                             \
        "vadd.vv v6, v5, v1\n"                                             \
        "vadd.vv v7, v1, v2\n"                                             \
        "vslide1down.vx v8, v7, %[x]\n"                                    \
        "vslide1down.vx v9, v1, %[x]\n"                                    \
        "vmin.vv v10, v9, v2\n"                                            \
        "vdiv.vv v11, v1, v2\n"                                            \
        "vadd.vv v12, v1, v2\n"                                            \
        "vse32.v v4, (%[o0])\n"                                            \
        "vse32.v v6, (%[o1])\n"                                            \
        "vse32.v v8, (%[o2])\n"                                            \
        "vse32.v v10, (%[o3])\n"                                           \
        "vse32.v v11, (%[o4])\n"                                           \
        "vse32.v v12, (%[o5])\n"                                           \
        : [vl] "=&r" (vl)                                                  \
        : [avl] "r" (ELEMENTS - done), [a] "r" (&inA[done]),               \
          [b] "r" (&inB[done]), [fa] "r" (&inFa[done]),                    \
          [fb] "r" (&inFb[done]), [x] "r" ((long)SLIDE_SCALAR),            \
          [o0] "r" (&out[0][done]), [o1] "r" (&out[1][done]),              \
          [o2] "r" (&out[2][done]), [o3] "r" (&out[3][done]),              \
          [o4] "r" (&out[4][done]), [o5] "r" (&out[5][done])               \
        : "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10",     \
          "v11", "v12", "v13", "v14", "memory")

/* Region of interest: all the strips. */
static __attribute__((noinline)) void
runKernel(void)
{
    size_t vl;
    for (size_t done = 0; done < ELEMENTS; done += vl) {
        CHAIN_BLOCK();
        if (vl == 0)
            break;
        stripLength[strips++] = vl;
    }
}

/*
 * Scalar reference of output o for element i, which is at position k of
 * a strip of length vl starting at element first.
 */
static int32_t
reference(unsigned o, size_t first, size_t k, size_t vl)
{
    const size_t i = first + k;
    const int32_t a = inA[i];
    const int32_t b = inB[i];
    switch (o) {
      case 0:
        return (int32_t)(bitsOf(inFa[i] * inFb[i]) + bitsOf(inFa[i]));
      case 1:
        return (int32_t)((uint32_t)(a / b) + (uint32_t)a);
      case 2:
        /* vslide1down: element k takes element k + 1; x goes to vl-1. */
        if (k + 1 < vl)
            return (int32_t)((uint32_t)inA[i + 1] + (uint32_t)inB[i + 1]);
        return SLIDE_SCALAR;
      case 3: {
        const int32_t slid = k + 1 < vl ? inA[i + 1] : SLIDE_SCALAR;
        return slid < b ? slid : b;
      }
      case 4:
        return a / b;
      default:
        return (int32_t)((uint32_t)a + (uint32_t)b);
    }
}

static int
reportFailure(const char *output, size_t i, long got, long expected)
{
    printText("[dependencies] FAIL: ");
    printText(output);
    printText("[");
    printInt((long)i);
    printText("]=");
    printInt(got);
    printText(", expected=");
    printInt(expected);
    printText("\n");
    return 1;
}

static int
verify(void)
{
    size_t first = 0;
    for (size_t s = 0; s < strips; ++s) {
        const size_t vl = stripLength[s];
        for (size_t k = 0; k < vl; ++k) {
            for (unsigned o = 0; o < OUTPUTS; ++o) {
                const int32_t expected = reference(o, first, k, vl);
                const int32_t got = out[o][first + k];
                if (got != expected)
                    return reportFailure(outputNames[o], first + k, got,
                                         expected);
            }
        }
        first += vl;
    }
    if (first != ELEMENTS)
        return reportFailure("vl total", 0, (long)first, ELEMENTS);
    return 0;
}

/* Fingerprint of all outputs, to compare modes. */
static void
printChecksum(void)
{
    uint64_t sum = 0;
    uint32_t xor_value = 0;
    for (unsigned o = 0; o < OUTPUTS; ++o) {
        for (size_t i = 0; i < ELEMENTS; ++i) {
            sum += (uint64_t)(uint32_t)out[o][i] * (i % 251 + 1);
            xor_value ^= (uint32_t)out[o][i] << (i % 8);
        }
    }
    printText("[dependencies] checksum: sum=");
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
    printText("[dependencies] PASS: 4 RAW chains and 1 independent pair, ");
    printInt(ELEMENTS);
    printText(" elements in ");
    printInt((long)strips);
    printText(" strips\n");
    return 0;
}
