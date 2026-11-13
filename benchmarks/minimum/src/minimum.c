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
 * Integer minimum: vmin.vv and vminu.vv with SEW 8/16/32/64 and LMUL m1
 * and m4, executed by the VPU ALU.
 *
 * Per LMUL, SEW and strip, one asm block loads A (vs2) and B (vs1) and
 * computes vmin.vv and vminu.vv. RVV 1.0 (edition 20240411), section
 * "Vector Integer Min/Max Instructions": vmin compares the elements as
 * two's complement integers and vminu as unsigned integers.
 *
 * The first elements are corner cases where both results differ (one
 * operand with the sign bit set) or are equal; the rest are
 * pseudo-random over the whole SEW range. ELEMENTS is larger than VLMAX
 * at SEW 8 with m4 (128 with VLEN 256), so every configuration has
 * several strips and a partial last strip. The VPU must enable SEW 8/64
 * and LMUL m4 with --vpu-set supported_sews and supported_lmuls; run.py
 * passes them.
 */

#include "vpu_runtime.h"

#ifndef ELEMENTS
#define ELEMENTS 150
#endif

#define ALIGNED __attribute__((aligned(64)))
#define WIDTHS 4
#define LMULS 2
/* Outputs per LMUL and SEW: vmin, then vminu. */
#define OUTPUTS 2

static const unsigned widths[WIDTHS] = {8, 16, 32, 64};
static const unsigned lmuls[LMULS] = {1, 4};
static const char *const outputNames[OUTPUTS] = {"vmin", "vminu"};

static uint8_t inA[WIDTHS][ELEMENTS * 8] ALIGNED;
static uint8_t inB[WIDTHS][ELEMENTS * 8] ALIGNED;
static uint8_t out[LMULS][WIDTHS][OUTPUTS][ELEMENTS * 8] ALIGNED;
static size_t covered[LMULS][WIDTHS];

static uint64_t
maskOf(unsigned sew)
{
    return sew == 64 ? ~0ull : (1ull << sew) - 1;
}

/* Two's complement value of the SEW low bits. */
static int64_t
signedOf(uint64_t value, unsigned sew)
{
    if (sew == 64)
        return (int64_t)value;
    const uint64_t sign = 1ull << (sew - 1);
    return (int64_t)((value ^ sign) - sign);
}

static uint64_t
loadElement(const uint8_t *array, size_t i, unsigned sew)
{
    uint64_t value = 0;
    for (unsigned k = 0; k < sew / 8; ++k)
        value |= (uint64_t)array[i * (sew / 8) + k] << (8 * k);
    return value;
}

static void
storeElement(uint8_t *array, size_t i, unsigned sew, uint64_t value)
{
    for (unsigned k = 0; k < sew / 8; ++k)
        array[i * (sew / 8) + k] = (uint8_t)(value >> (8 * k));
}

/* Deterministic LCG; the low bits are discarded. */
static uint32_t
nextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

#define CORNER_COUNT 10

/*
 * Corner case i as (a, b) for one SEW. A switch instead of a local
 * table: Clang lowers a table of constants with RVV instructions outside
 * the kernel, which check_profile.py rejects.
 */
static void
corner(size_t i, unsigned sew, uint64_t *a, uint64_t *b)
{
    const uint64_t mask = maskOf(sew);
    const uint64_t min = 1ull << (sew - 1);
    const uint64_t max = min - 1;
    switch (i) {
      case 0: *a = min; *b = 1; break;           /* vmin: a, vminu: b */
      case 1: *a = 1; *b = min; break;
      case 2: *a = mask; *b = 0; break;          /* -1 against 0 */
      case 3: *a = max; *b = min; break;         /* largest gap */
      case 4: *a = (uint64_t)-5; *b = (uint64_t)-3; break;
      case 5: *a = 7; *b = 3; break;             /* both agree */
      case 6: *a = mask; *b = mask; break;       /* equal */
      case 7: *a = 0; *b = max; break;
      case 8: *a = min; *b = mask; break;
      default: *a = max; *b = mask; break;
    }
    *a &= mask;
    *b &= mask;
}

/* Corner cases first, then random values over the whole SEW range. */
static void
initData(void)
{
    uint32_t state = 2026u;
    for (unsigned w = 0; w < WIDTHS; ++w) {
        const unsigned sew = widths[w];
        const uint64_t mask = maskOf(sew);
        for (size_t i = 0; i < ELEMENTS; ++i) {
            uint64_t a;
            uint64_t b;
            if (i < CORNER_COUNT) {
                corner(i, sew, &a, &b);
            } else {
                a = ((uint64_t)nextRandom(&state) << 40) ^
                    ((uint64_t)nextRandom(&state) << 16) ^
                    nextRandom(&state);
                b = ((uint64_t)nextRandom(&state) << 40) ^
                    ((uint64_t)nextRandom(&state) << 16) ^
                    nextRandom(&state);
            }
            storeElement(inA[w], i, sew, a & mask);
            storeElement(inB[w], i, sew, b & mask);
        }
    }
}

/*
 * One strip of one LMUL and SEW. The registers are multiples of 4, so
 * the same block is legal with m1 and m4. Always inlined into
 * runKernel(), as check_profile.py requires.
 */
#define MIN_BLOCK(sew, lmul)                                               \
    __asm__ volatile (                                                     \
        "vsetvli %[vl], %[avl], e" #sew ", m" #lmul ", tu, mu\n"           \
        "vle" #sew ".v v4, (%[a])\n"                                       \
        "vle" #sew ".v v8, (%[b])\n"                                       \
        "vmin.vv v12, v4, v8\n"                                            \
        "vminu.vv v16, v4, v8\n"                                           \
        "vse" #sew ".v v12, (%[o0])\n"                                     \
        "vse" #sew ".v v16, (%[o1])\n"                                     \
        : [vl] "=&r" (vl)                                                  \
        : [avl] "r" (ELEMENTS - done), [a] "r" (&inA[w][offset]),          \
          [b] "r" (&inB[w][offset]), [o0] "r" (&out[l][w][0][offset]),     \
          [o1] "r" (&out[l][w][1][offset])                                 \
        : "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13",  \
          "v14", "v15", "v16", "v17", "v18", "v19", "memory")

#define RUN(sew, lmul, w_index, l_index)                                   \
    {                                                                      \
        const unsigned w = (w_index);                                      \
        const unsigned l = (l_index);                                      \
        size_t vl;                                                         \
        for (size_t done = 0; done < ELEMENTS; done += vl) {               \
            const size_t offset = done * ((sew) / 8);                      \
            MIN_BLOCK(sew, lmul);                                          \
            covered[l][w] += vl;                                           \
            if (vl == 0)                                                   \
                break;                                                     \
        }                                                                  \
    }

/* Region of interest: m1 and then m4, the four SEW in order. */
static __attribute__((noinline)) void
runKernel(void)
{
    RUN(8, 1, 0, 0)
    RUN(16, 1, 1, 0)
    RUN(32, 1, 2, 0)
    RUN(64, 1, 3, 0)
    RUN(8, 4, 0, 1)
    RUN(16, 4, 1, 1)
    RUN(32, 4, 2, 1)
    RUN(64, 4, 3, 1)
}

/* Scalar reference of RVV 1.0: index 0 is vmin, 1 is vminu. */
static uint64_t
reference(unsigned output, uint64_t a, uint64_t b, unsigned sew)
{
    if (output == 0)
        return signedOf(a, sew) <= signedOf(b, sew) ? a : b;
    return a <= b ? a : b;
}

static int
reportFailure(unsigned lmul, unsigned sew, const char *operation, size_t i,
              uint64_t got, uint64_t expected)
{
    printText("[minimum] FAIL: m");
    printInt(lmul);
    printText(" e");
    printInt(sew);
    printText(" ");
    printText(operation);
    printText("[");
    printInt((long)i);
    printText("]=");
    printInt((long)got);
    printText(", expected=");
    printInt((long)expected);
    printText("\n");
    return 1;
}

static int
verify(void)
{
    for (unsigned l = 0; l < LMULS; ++l) {
        for (unsigned w = 0; w < WIDTHS; ++w) {
            const unsigned sew = widths[w];
            if (covered[l][w] != ELEMENTS)
                return reportFailure(lmuls[l], sew, "vl total", 0,
                                     covered[l][w], ELEMENTS);
            for (size_t i = 0; i < ELEMENTS; ++i) {
                const uint64_t a = loadElement(inA[w], i, sew);
                const uint64_t b = loadElement(inB[w], i, sew);
                for (unsigned o = 0; o < OUTPUTS; ++o) {
                    const uint64_t expected = reference(o, a, b, sew);
                    const uint64_t got = loadElement(out[l][w][o], i, sew);
                    if (got != expected)
                        return reportFailure(lmuls[l], sew, outputNames[o],
                                             i, got, expected);
                }
            }
        }
    }
    return 0;
}

/* Fingerprint of all outputs, to compare modes. */
static void
printChecksum(void)
{
    uint64_t sum = 0;
    uint32_t xor_value = 0;
    for (unsigned l = 0; l < LMULS; ++l) {
        for (unsigned w = 0; w < WIDTHS; ++w) {
            for (unsigned o = 0; o < OUTPUTS; ++o) {
                for (size_t i = 0; i < ELEMENTS * (widths[w] / 8); ++i) {
                    sum += (uint64_t)out[l][w][o][i] * (i % 251 + 1);
                    xor_value ^= (uint32_t)out[l][w][o][i] << (8 * (i % 4));
                }
            }
        }
    }
    printText("[minimum] checksum: sum=");
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
    printText("[minimum] PASS: vmin.vv and vminu.vv, SEW 8/16/32/64, "
              "m1 and m4, ");
    printInt(ELEMENTS);
    printText(" elements\n");
    return 0;
}
