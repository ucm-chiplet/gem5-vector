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
 * Widths and register groups (criteria 3 and 10 of hito 2): integer add
 * and unit-stride memory with SEW 8/16/32/64 and every LMUL of the
 * target profile, that is m1/m2/m4/m8 and the fractional mf2/mf4/mf8
 * allowed by SEW <= LMUL * ELEN (ELEN=64): 22 combinations.
 *
 * For each combination a strip-mined loop runs
 *   vle<SEW>.v  v8,  (A)
 *   vle<SEW>.v  v16, (B)
 *   vadd.vv     v24, v8, v16      O0[i] = A[i] + B[i]   mod 2^SEW
 *   vse<SEW>.v  v24, (O0)
 *   vadd.vx     v24, v8, x        O1[i] = A[i] + x      mod 2^SEW
 *   vse<SEW>.v  v24, (O1)
 * v8, v16 and v24 are aligned for any LMUL up to m8. x is a 64-bit
 * constant: with SEW < XLEN only its SEW low bits are used.
 *
 * Every array starts 24 bytes before a 4 KiB page boundary, so the first
 * strip crosses a page and its lines; ELEMENTS is larger than the largest
 * VLMAX (SEW=8, m8: 256) and not a multiple of any VLMAX, so there are
 * several strips and a partial last strip. Bytes after each output are
 * filled with a sentinel that must stay untouched (access range).
 *
 * RVV 1.0 (edition 20240411): Vector Single-Width Integer Add and
 * Subtract, Vector Unit-Stride Instructions, Vector Register Grouping
 * (vlmul) and Configuration-Setting Instructions. The VPU must be built
 * with --vpu-set supported_sews=8,16,32,64 and supported_lmuls=-3..3;
 * run.py passes them.
 */

#include "vpu_runtime.h"

#ifndef ELEMENTS
#define ELEMENTS 300
#endif

#define PAGE 4096
/* Start of each array inside its segment: 24 bytes before a page end. */
#define ARRAY_OFFSET (PAGE - 24)
#define GUARD 64
#define SEGMENT (2 * PAGE + ((ELEMENTS * 8 + GUARD + PAGE - 1) / PAGE) * PAGE)
#define SENTINEL 0xa5
/* Scalar operand of vadd.vx; each SEW uses its low bits. */
#define SCALAR 0x0123456789abcdefull

/* Same list as the profile: SEW, LMUL mnemonic. */
#define COMBINATIONS(X) \
    X(8, mf8) X(8, mf4) X(8, mf2) X(8, m1) X(8, m2) X(8, m4) X(8, m8) \
    X(16, mf4) X(16, mf2) X(16, m1) X(16, m2) X(16, m4) X(16, m8) \
    X(32, mf2) X(32, m1) X(32, m2) X(32, m4) X(32, m8) \
    X(64, m1) X(64, m2) X(64, m4) X(64, m8)

#define COUNT_ONE(sew, lmul) +1
enum { COMBINATION_COUNT = 0 COMBINATIONS(COUNT_ONE) };

/* Page-aligned segments: inputs A and B, then two outputs per combination. */
static uint8_t pool[2 + 2 * COMBINATION_COUNT][SEGMENT]
    __attribute__((aligned(PAGE)));

static const char *const combinationNames[COMBINATION_COUNT] = {
#define NAME_ONE(sew, lmul) "e" #sew " " #lmul,
    COMBINATIONS(NAME_ONE)
#undef NAME_ONE
};
static const unsigned combinationSew[COMBINATION_COUNT] = {
#define SEW_ONE(sew, lmul) sew,
    COMBINATIONS(SEW_ONE)
#undef SEW_ONE
};
/* Total elements covered by the strips of each combination, and strips. */
static size_t coveredElements[COMBINATION_COUNT];
static size_t stripCount[COMBINATION_COUNT];

static uint8_t *
arrayOf(size_t segment)
{
    return &pool[segment][ARRAY_OFFSET];
}

/* Deterministic LCG; the low bits are discarded. */
static uint32_t
nextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

/* Random bytes in the inputs and the sentinel everywhere else. */
static void
initData(void)
{
    uint32_t state = 777u;
    for (size_t segment = 0; segment < 2 + 2 * COMBINATION_COUNT;
         ++segment) {
        for (size_t i = 0; i < SEGMENT; ++i)
            pool[segment][i] = SENTINEL;
    }
    for (size_t i = 0; i < ELEMENTS * 8; ++i) {
        arrayOf(0)[i] = (uint8_t)nextRandom(&state);
        arrayOf(1)[i] = (uint8_t)nextRandom(&state);
    }
}

/*
 * One combination. Always inlined so that every RVV instruction belongs
 * to runKernel(). v8-v31 are clobbered because an m8 group spans eight
 * registers from v8, v16 and v24.
 */
#define RUN_ONE(sew, lmul)                                                 \
    {                                                                      \
        const size_t bytes = (sew) / 8;                                    \
        uint8_t *out0 = arrayOf(2 + 2 * index);                            \
        uint8_t *out1 = arrayOf(3 + 2 * index);                            \
        size_t vl;                                                         \
        for (size_t done = 0; done < ELEMENTS; done += vl) {               \
            __asm__ volatile (                                             \
                "vsetvli %[vl], %[avl], e" #sew ", " #lmul ", tu, mu\n"    \
                "vle" #sew ".v v8, (%[a])\n"                               \
                "vle" #sew ".v v16, (%[b])\n"                              \
                "vadd.vv v24, v8, v16\n"                                   \
                "vse" #sew ".v v24, (%[o0])\n"                             \
                "vadd.vx v24, v8, %[x]\n"                                  \
                "vse" #sew ".v v24, (%[o1])\n"                             \
                : [vl] "=&r" (vl)                                          \
                : [avl] "r" (ELEMENTS - done),                             \
                  [a] "r" (arrayOf(0) + done * bytes),                     \
                  [b] "r" (arrayOf(1) + done * bytes),                     \
                  [o0] "r" (out0 + done * bytes),                          \
                  [o1] "r" (out1 + done * bytes), [x] "r" (SCALAR)         \
                : "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",    \
                  "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",  \
                  "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",  \
                  "memory");                                               \
            coveredElements[index] += vl;                                  \
            ++stripCount[index];                                           \
            if (vl == 0)                                                   \
                break;                                                     \
        }                                                                  \
        ++index;                                                           \
    }

/* Region of interest: the 22 combinations in profile order. */
static __attribute__((noinline)) void
runKernel(void)
{
    size_t index = 0;
    COMBINATIONS(RUN_ONE)
}

/* Little-endian element of a byte array. */
static uint64_t
loadElement(const uint8_t *array, size_t i, size_t bytes)
{
    uint64_t value = 0;
    for (size_t k = 0; k < bytes; ++k)
        value |= (uint64_t)array[i * bytes + k] << (8 * k);
    return value;
}

static int
reportFailure(size_t combination, const char *what, size_t i, uint64_t got,
              uint64_t expected)
{
    printText("[widths] FAIL: ");
    printText(combinationNames[combination]);
    printText(" ");
    printText(what);
    printText("[");
    printInt((long)i);
    printText("]=");
    printInt((long)got);
    printText(", expected=");
    printInt((long)expected);
    printText("\n");
    return 1;
}

/* Scalar reference modulo 2^SEW, then the guard bytes after each output. */
static int
verify(void)
{
    for (size_t c = 0; c < COMBINATION_COUNT; ++c) {
        const size_t bytes = combinationSew[c] / 8;
        const uint64_t mask =
            bytes == 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
        const uint8_t *out0 = arrayOf(2 + 2 * c);
        const uint8_t *out1 = arrayOf(3 + 2 * c);
        if (coveredElements[c] != ELEMENTS)
            return reportFailure(c, "vl total", 0, coveredElements[c],
                                 ELEMENTS);
        for (size_t i = 0; i < ELEMENTS; ++i) {
            const uint64_t a = loadElement(arrayOf(0), i, bytes);
            const uint64_t b = loadElement(arrayOf(1), i, bytes);
            const uint64_t sum = (a + b) & mask;
            const uint64_t sum_x = (a + SCALAR) & mask;
            if (loadElement(out0, i, bytes) != sum)
                return reportFailure(c, "O0", i, loadElement(out0, i, bytes),
                                     sum);
            if (loadElement(out1, i, bytes) != sum_x)
                return reportFailure(c, "O1", i, loadElement(out1, i, bytes),
                                     sum_x);
        }
        for (size_t k = 0; k < GUARD; ++k) {
            const size_t position = ELEMENTS * bytes + k;
            if (out0[position] != SENTINEL || out1[position] != SENTINEL)
                return reportFailure(c, "guard", k, out0[position],
                                     SENTINEL);
        }
    }
    return 0;
}

/* Fingerprint of all outputs and strip counts, to compare modes. */
static void
printChecksum(void)
{
    uint64_t sum = 0;
    uint32_t xor_value = 0;
    size_t strips = 0;
    for (size_t c = 0; c < COMBINATION_COUNT; ++c) {
        const size_t bytes = combinationSew[c] / 8;
        for (size_t i = 0; i < ELEMENTS * bytes; ++i) {
            const uint8_t v0 = arrayOf(2 + 2 * c)[i];
            const uint8_t v1 = arrayOf(3 + 2 * c)[i];
            sum += (uint64_t)(v0 + v1) * (i % 251 + 1);
            xor_value ^= (uint32_t)(v0 ^ v1) << (8 * (i % 4));
        }
        strips += stripCount[c];
    }
    printText("[widths] checksum: sum=");
    printInt((long)sum);
    printText(" xor=");
    printInt((long)xor_value);
    printText(" strips=");
    printInt((long)strips);
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
    printText("[widths] PASS: ");
    printInt(COMBINATION_COUNT);
    printText(" SEW/LMUL combinations, ");
    printInt(ELEMENTS);
    printText(" elements each\n");
    return 0;
}
