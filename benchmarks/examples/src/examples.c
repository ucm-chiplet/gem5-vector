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
 * Closure examples of hito 2, taken instruction by instruction from
 * "Ejemplos al cierre: enteros de 16 bits y FP32" in
 * documentacion/Plan de evolución - Hitos 2 a 5.md.
 *
 * Variant int16 (default): with SEW=16, LMUL=m1,
 *   S[i] = A[i] + B[i] mod 2^16       (vadd.vv)
 *   P[i] = A[i] * B[i] mod 2^16       (vmul.vv, low 16 bits)
 *   D[i] = A[i+1], 0 at the end       (vslide1down.vx with x0)
 * Variant fp32 (-DFP32=1): with SEW=32, LMUL=m1,
 *   P[i] = A[i] * B[i]                (vfmul.vv, RNE)
 *   S[i] = A[i] + B[i]                (vfadd.vv, RNE)
 *
 * The document shows one block of t0 elements. Here the block runs in a
 * strip-mining loop: a0 is the remaining count and each iteration
 * advances the pointers by vl. Each strip is one vslide1down, so the
 * element inserted at the end of every strip is 0, not the first element
 * of the next strip; the reference reproduces that using the vl values
 * recorded per strip (RVV lets vl be any legal value for a given AVL).
 *
 * Two cases run inside runKernel():
 *   1. The document case, 3 elements, checked against the exact values
 *      written in the document.
 *   2. ELEMENTS pseudo-random elements: several strips, a partial last
 *      strip (tail) and slides that cross lanes. In int16 the values cover
 *      the whole 16-bit range, so sums and products wrap. In fp32 they are
 *      checked bit by bit against scalar F instructions in RNE.
 *
 * RVV 1.0 (edition 20240411): Vector Single-Width Integer Add and Subtract,
 * Vector Single-Width Integer Multiply Instructions, Vector Slide1down,
 * Vector Single-Width Floating-Point Add/Subtract and Multiply/Divide
 * Instructions; vsetvli and tail/mask policies (tu, mu) in Configuration-
 * Setting Instructions.
 */

#include "vpu_runtime.h"

#ifndef ELEMENTS
#define ELEMENTS 100
#endif

#ifndef FP32
#define FP32 0
#endif

#define DOC_ELEMENTS 3
/* Worst case strips: vl is at least 1 while elements remain. */
#define MAX_STRIPS (ELEMENTS + DOC_ELEMENTS)
#define ALIGNED __attribute__((aligned(64)))

#if FP32
typedef float element_t;
#define NAME "[examples]"
#define VARIANT_TEXT "fp32"
#else
typedef uint16_t element_t;
#define NAME "[examples]"
#define VARIANT_TEXT "int16"
#endif

/* Inputs and outputs of both cases; o2 is only used by int16 (D). */
static element_t docA[DOC_ELEMENTS] ALIGNED;
static element_t docB[DOC_ELEMENTS] ALIGNED;
static element_t docO0[DOC_ELEMENTS] ALIGNED;
static element_t docO1[DOC_ELEMENTS] ALIGNED;
static element_t docO2[DOC_ELEMENTS] ALIGNED;
static element_t a[ELEMENTS] ALIGNED;
static element_t b[ELEMENTS] ALIGNED;
static element_t o0[ELEMENTS] ALIGNED;
static element_t o1[ELEMENTS] ALIGNED;
static element_t o2[ELEMENTS] ALIGNED;

/* vl returned by vsetvli in each strip of each case. */
static size_t docStrips[MAX_STRIPS];
static size_t docStripCount;
static size_t strips[MAX_STRIPS];
static size_t stripCount;

/* Deterministic LCG; the low bits are discarded. */
static uint32_t
nextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static void
initData(void)
{
    uint32_t state = 2026u;
#if FP32
    /* Document values: A=[1,2,3], B=[0.5,1.5,2]. */
    docA[0] = 1.0f;
    docA[1] = 2.0f;
    docA[2] = 3.0f;
    docB[0] = 0.5f;
    docB[1] = 1.5f;
    docB[2] = 2.0f;
    /*
     * Quotients of integers by 7 have long mantissas, so products and
     * sums need rounding: the check compares the rounded bits.
     */
    for (size_t i = 0; i < ELEMENTS; ++i) {
        a[i] = (float)((int32_t)(nextRandom(&state) % 20000u) - 10000) / 7.0f;
        b[i] = (float)((int32_t)(nextRandom(&state) % 20000u) - 10000) / 7.0f;
    }
#else
    /* Document values: A=[100,200,300], B=[2,3,4]. */
    docA[0] = 100;
    docA[1] = 200;
    docA[2] = 300;
    docB[0] = 2;
    docB[1] = 3;
    docB[2] = 4;
    /* Full 16-bit range: sums and products wrap modulo 2^16. */
    for (size_t i = 0; i < ELEMENTS; ++i) {
        a[i] = (uint16_t)nextRandom(&state);
        b[i] = (uint16_t)nextRandom(&state);
    }
#endif
}

/*
 * One strip-mined pass of the document block over n elements. It is
 * always inlined so that every RVV instruction belongs to runKernel(),
 * which check_profile.py requires. Operand names map to the document
 * registers: vl is t0, avl is a0 and the pointers are a1..a5.
 */
static inline __attribute__((always_inline)) size_t
runBlock(size_t n, const element_t *srcA, const element_t *srcB,
         element_t *out0, element_t *out1, element_t *out2, size_t *vlLog)
{
    size_t count = 0;
    size_t vl;
    for (size_t done = 0; done < n; done += vl) {
#if FP32
        (void)out2;
        __asm__ volatile (
            "vsetvli %[vl], %[avl], e32, m1, tu, mu\n"
            "vle32.v v1, (%[a])\n"
            "vle32.v v2, (%[b])\n"
            "vfmul.vv v3, v1, v2\n"
            "vfadd.vv v4, v1, v2\n"
            "vse32.v v3, (%[p])\n"
            "vse32.v v4, (%[s])\n"
            : [vl] "=&r" (vl)
            : [avl] "r" (n - done), [a] "r" (&srcA[done]),
              [b] "r" (&srcB[done]), [p] "r" (&out0[done]),
              [s] "r" (&out1[done])
            : "v1", "v2", "v3", "v4", "memory");
#else
        __asm__ volatile (
            "vsetvli %[vl], %[avl], e16, m1, tu, mu\n"
            "vle16.v v1, (%[a])\n"
            "vle16.v v2, (%[b])\n"
            "vadd.vv v3, v1, v2\n"
            "vmul.vv v4, v1, v2\n"
            "vslide1down.vx v5, v1, zero\n"
            "vse16.v v3, (%[s])\n"
            "vse16.v v4, (%[p])\n"
            "vse16.v v5, (%[d])\n"
            : [vl] "=&r" (vl)
            : [avl] "r" (n - done), [a] "r" (&srcA[done]),
              [b] "r" (&srcB[done]), [s] "r" (&out0[done]),
              [p] "r" (&out1[done]), [d] "r" (&out2[done])
            : "v1", "v2", "v3", "v4", "v5", "memory");
#endif
        vlLog[count++] = vl;
    }
    return count;
}

/* Region of interest: both cases, nothing else. */
static __attribute__((noinline)) void
runKernel(void)
{
    docStripCount =
        runBlock(DOC_ELEMENTS, docA, docB, docO0, docO1, docO2, docStrips);
    stripCount = runBlock(ELEMENTS, a, b, o0, o1, o2, strips);
}

#if FP32
/* Raw bits of a float, without type punning through pointers. */
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
#else
static uint32_t
bitsOf(uint16_t value)
{
    return value;
}
#endif

/* Prints the first mismatch of one output and returns 1, or returns 0. */
static int
checkValue(const char *name, size_t i, uint32_t got, uint32_t expected)
{
    if (got == expected)
        return 0;
    printText(NAME " FAIL: ");
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

/* The vl values of one case must cover exactly n elements. */
static int
checkStrips(const char *name, const size_t *vlLog, size_t count, size_t n)
{
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (vlLog[i] == 0)
            return checkValue(name, i, 0, 1);
        total += vlLog[i];
    }
    return checkValue(name, count, (uint32_t)total, (uint32_t)n);
}

#if FP32
/* The document case with the exact values of the document. */
static int
verifyDocument(void)
{
    static const float expectedP[DOC_ELEMENTS] = {0.5f, 3.0f, 6.0f};
    static const float expectedS[DOC_ELEMENTS] = {1.5f, 3.5f, 5.0f};
    for (size_t i = 0; i < DOC_ELEMENTS; ++i) {
        if (checkValue("doc P", i, bitsOf(docO0[i]), bitsOf(expectedP[i])) ||
            checkValue("doc S", i, bitsOf(docO1[i]), bitsOf(expectedS[i])))
            return 1;
    }
    return 0;
}

/* Scalar reference: F instructions in RNE, compared bit by bit. */
static int
verifyRandom(void)
{
    for (size_t i = 0; i < ELEMENTS; ++i) {
        if (checkValue("P", i, bitsOf(o0[i]), bitsOf(a[i] * b[i])) ||
            checkValue("S", i, bitsOf(o1[i]), bitsOf(a[i] + b[i])))
            return 1;
    }
    return 0;
}
#else
static int
verifyDocument(void)
{
    static const uint16_t expectedS[DOC_ELEMENTS] = {102, 203, 304};
    static const uint16_t expectedP[DOC_ELEMENTS] = {200, 600, 1200};
    static const uint16_t expectedD[DOC_ELEMENTS] = {200, 300, 0};
    for (size_t i = 0; i < DOC_ELEMENTS; ++i) {
        if (checkValue("doc S", i, docO0[i], expectedS[i]) ||
            checkValue("doc P", i, docO1[i], expectedP[i]) ||
            checkValue("doc D", i, docO2[i], expectedD[i]))
            return 1;
    }
    return 0;
}

/*
 * Scalar reference modulo 2^16. Inside each strip D[i] = A[i+1]; the last
 * element of the strip receives the scalar x0 = 0.
 */
static int
verifyRandom(void)
{
    size_t start = 0;
    for (size_t strip = 0; strip < stripCount; ++strip) {
        const size_t end = start + strips[strip];
        for (size_t i = start; i < end; ++i) {
            const uint16_t sum = (uint16_t)(a[i] + b[i]);
            const uint16_t product = (uint16_t)((uint32_t)a[i] * b[i]);
            const uint16_t down = i + 1 < end ? a[i + 1] : 0;
            if (checkValue("S", i, o0[i], sum) ||
                checkValue("P", i, o1[i], product) ||
                checkValue("D", i, o2[i], down))
                return 1;
        }
        start = end;
    }
    return 0;
}
#endif

/* Fingerprint of all outputs and of the strip sizes, to compare modes. */
static void
printChecksum(void)
{
    uint64_t sum = 0;
    uint32_t xor_value = 0;
    for (size_t i = 0; i < DOC_ELEMENTS; ++i) {
        sum += bitsOf(docO0[i]) + bitsOf(docO1[i]) + bitsOf(docO2[i]);
        xor_value ^= bitsOf(docO0[i]) ^ bitsOf(docO1[i]) ^ bitsOf(docO2[i]);
    }
    for (size_t i = 0; i < ELEMENTS; ++i) {
        sum += bitsOf(o0[i]) + bitsOf(o1[i]) + bitsOf(o2[i]);
        xor_value ^= bitsOf(o0[i]) ^ bitsOf(o1[i]) ^ bitsOf(o2[i]);
    }
    printText(NAME " checksum: sum=");
    printInt((long)sum);
    printText(" xor=");
    printInt((long)xor_value);
    printText(" strips=");
    printInt((long)stripCount);
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
    if (checkStrips("doc vl", docStrips, docStripCount, DOC_ELEMENTS) ||
        checkStrips("vl", strips, stripCount, ELEMENTS) ||
        verifyDocument() || verifyRandom())
        return 1;
    printText(NAME " PASS: " VARIANT_TEXT ", document case and ");
    printInt(ELEMENTS);
    printText(" elements\n");
    return 0;
}
