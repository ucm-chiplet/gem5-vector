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
 * Integer multiply, divide and remainder (criterion 13 of hito 2): the
 * .vv and .vx forms of vmul, vdiv, vdivu, vrem and vremu with SEW
 * 8/16/32/64 and LMUL=m1, executed by the VPU ALU.
 *
 * Per SEW and strip, one asm block runs the five .vv forms over A (vs2)
 * and B (vs1), and three more run the five .vx forms over A with the
 * scalars 0, -1 and -3 (in a register, not x0). RVV computes vd = vs2 op
 * vs1 (or rs1); with SEW < XLEN only the SEW low bits of rs1 are used.
 *
 * Results fixed by RVV 1.0 (edition 20240411), section "Vector Integer
 * Divide Instructions", which the scalar reference reproduces:
 *   division by zero: vdivu -> 2^SEW - 1, vdiv -> -1,
 *                     vremu -> dividend, vrem -> dividend;
 *   overflow (-2^(SEW-1) / -1): vdiv -> -2^(SEW-1), vrem -> 0.
 * vmul keeps the SEW low bits of the product ("Vector Single-Width
 * Integer Multiply Instructions").
 *
 * The first elements of A and B are those corner cases; the rest are
 * pseudo-random over the whole SEW range (B may also be zero). ELEMENTS
 * is larger than VLMAX at SEW=8 (32), so there are several strips and a
 * partial last strip. The VPU must enable SEW 8 and 64 with
 * --vpu-set supported_sews=8,16,32,64; run.py passes it.
 */

#include "vpu_runtime.h"

#ifndef ELEMENTS
#define ELEMENTS 40
#endif

#define ALIGNED __attribute__((aligned(64)))
#define WIDTHS 4
/* Outputs per SEW: 5 .vv forms, then 5 .vx forms for each scalar. */
#define OPS 5
#define SCALARS 3
#define OUTPUTS (OPS + OPS * SCALARS)

enum Operation { Mul, Div, Divu, Rem, Remu };
static const char *const operationNames[OPS] = {"vmul", "vdiv", "vdivu",
                                                "vrem", "vremu"};
static const unsigned widths[WIDTHS] = {8, 16, 32, 64};
static const int64_t scalars[SCALARS] = {0, -1, -3};

static uint8_t inA[WIDTHS][ELEMENTS * 8] ALIGNED;
static uint8_t inB[WIDTHS][ELEMENTS * 8] ALIGNED;
static uint8_t out[WIDTHS][OUTPUTS][ELEMENTS * 8] ALIGNED;
static size_t covered[WIDTHS];

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

#define CORNER_COUNT 12

/*
 * Corner case i as (dividend, divisor) for one SEW. A switch instead of a
 * local table: Clang lowers a table of constants with RVV instructions
 * outside the kernel, which check_profile.py rejects.
 */
static void
corner(size_t i, unsigned sew, uint64_t *a, uint64_t *b)
{
    const uint64_t mask = maskOf(sew);
    const uint64_t min = 1ull << (sew - 1);
    const uint64_t max = min - 1;
    switch (i) {
      case 0: *a = min; *b = mask; break;           /* overflow */
      case 1: *a = min; *b = 0; break;              /* by zero */
      case 2: *a = 5; *b = 0; break;
      case 3: *a = 0; *b = 0; break;
      case 4: *a = (uint64_t)-7; *b = 2; break;     /* signs */
      case 5: *a = 7; *b = (uint64_t)-2; break;
      case 6: *a = (uint64_t)-7; *b = (uint64_t)-2; break;
      case 7: *a = max; *b = mask; break;
      case 8: *a = mask; *b = min; break;
      case 9: *a = min; *b = 1; break;
      case 10: *a = max; *b = max; break;
      default: *a = min; *b = min; break;
    }
    *a &= mask;
    *b &= mask;
}

/* Corner cases first, then random values over the whole SEW range. */
static void
initData(void)
{
    uint32_t state = 4242u;
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
                /* Small divisors too, so quotients are not almost 0. */
                if (i % 3 == 0)
                    b = (b % 17) - 8;
            }
            storeElement(inA[w], i, sew, a & mask);
            storeElement(inB[w], i, sew, b & mask);
        }
    }
}

/*
 * One strip of one SEW: .vv block with v1 = A, v2 = B, then one .vx block
 * per scalar. Always inlined into runKernel(), as check_profile.py
 * requires.
 */
#define VV_BLOCK(sew)                                                      \
    __asm__ volatile (                                                     \
        "vsetvli %[vl], %[avl], e" #sew ", m1, tu, mu\n"                   \
        "vle" #sew ".v v1, (%[a])\n"                                       \
        "vle" #sew ".v v2, (%[b])\n"                                       \
        "vmul.vv v3, v1, v2\n"                                             \
        "vdiv.vv v4, v1, v2\n"                                             \
        "vdivu.vv v5, v1, v2\n"                                            \
        "vrem.vv v6, v1, v2\n"                                             \
        "vremu.vv v7, v1, v2\n"                                            \
        "vse" #sew ".v v3, (%[o0])\n"                                      \
        "vse" #sew ".v v4, (%[o1])\n"                                      \
        "vse" #sew ".v v5, (%[o2])\n"                                      \
        "vse" #sew ".v v6, (%[o3])\n"                                      \
        "vse" #sew ".v v7, (%[o4])\n"                                      \
        : [vl] "=&r" (vl)                                                  \
        : [avl] "r" (ELEMENTS - done), [a] "r" (&inA[w][offset]),          \
          [b] "r" (&inB[w][offset]), [o0] "r" (&out[w][0][offset]),        \
          [o1] "r" (&out[w][1][offset]), [o2] "r" (&out[w][2][offset]),    \
          [o3] "r" (&out[w][3][offset]), [o4] "r" (&out[w][4][offset])     \
        : "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory")

#define VX_BLOCK(sew, s)                                                   \
    __asm__ volatile (                                                     \
        "vsetvli %[vl], %[avl], e" #sew ", m1, tu, mu\n"                   \
        "vle" #sew ".v v1, (%[a])\n"                                       \
        "vmul.vx v3, v1, %[x]\n"                                           \
        "vdiv.vx v4, v1, %[x]\n"                                           \
        "vdivu.vx v5, v1, %[x]\n"                                          \
        "vrem.vx v6, v1, %[x]\n"                                           \
        "vremu.vx v7, v1, %[x]\n"                                          \
        "vse" #sew ".v v3, (%[o0])\n"                                      \
        "vse" #sew ".v v4, (%[o1])\n"                                      \
        "vse" #sew ".v v5, (%[o2])\n"                                      \
        "vse" #sew ".v v6, (%[o3])\n"                                      \
        "vse" #sew ".v v7, (%[o4])\n"                                      \
        : [vl] "=&r" (vl)                                                  \
        : [avl] "r" (ELEMENTS - done), [a] "r" (&inA[w][offset]),          \
          [x] "r" (scalars[s]),                                            \
          [o0] "r" (&out[w][OPS * (s + 1) + 0][offset]),                   \
          [o1] "r" (&out[w][OPS * (s + 1) + 1][offset]),                   \
          [o2] "r" (&out[w][OPS * (s + 1) + 2][offset]),                   \
          [o3] "r" (&out[w][OPS * (s + 1) + 3][offset]),                   \
          [o4] "r" (&out[w][OPS * (s + 1) + 4][offset])                    \
        : "v1", "v3", "v4", "v5", "v6", "v7", "memory")

/*
 * The four blocks of a strip get the same AVL, so vsetvli returns the
 * same vl in all of them; the loop advances by the last one.
 */
#define RUN_WIDTH(sew, index)                                              \
    {                                                                      \
        const unsigned w = (index);                                        \
        size_t vl;                                                         \
        for (size_t done = 0; done < ELEMENTS; done += vl) {               \
            const size_t offset = done * ((sew) / 8);                      \
            VV_BLOCK(sew);                                                 \
            VX_BLOCK(sew, 0);                                              \
            VX_BLOCK(sew, 1);                                              \
            VX_BLOCK(sew, 2);                                              \
            covered[w] += vl;                                              \
            if (vl == 0)                                                   \
                break;                                                     \
        }                                                                  \
    }

/* Region of interest: the four SEW in order. */
static __attribute__((noinline)) void
runKernel(void)
{
    RUN_WIDTH(8, 0)
    RUN_WIDTH(16, 1)
    RUN_WIDTH(32, 2)
    RUN_WIDTH(64, 3)
}

/* Scalar reference of RVV 1.0, including division by zero and overflow. */
static uint64_t
reference(enum Operation op, uint64_t a, uint64_t b, unsigned sew)
{
    const uint64_t mask = maskOf(sew);
    const int64_t sa = signedOf(a, sew);
    const int64_t sb = signedOf(b, sew);
    const int overflow =
        sa == signedOf(1ull << (sew - 1), sew) && sb == -1;
    switch (op) {
      case Mul:
        return (a * b) & mask;
      case Divu:
        return b == 0 ? mask : a / b;
      case Remu:
        return b == 0 ? a : a % b;
      case Div:
        if (b == 0)
            return mask;
        if (overflow)
            return a;
        return (uint64_t)(sa / sb) & mask;
      case Rem:
        if (b == 0)
            return a;
        if (overflow)
            return 0;
        return (uint64_t)(sa % sb) & mask;
    }
    return 0;
}

static int
reportFailure(unsigned sew, const char *operation, const char *form,
              size_t i, uint64_t got, uint64_t expected)
{
    printText("[muldiv] FAIL: e");
    printInt(sew);
    printText(" ");
    printText(operation);
    printText(form);
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
    static const char *const forms[SCALARS + 1] = {
        ".vv", ".vx(0)", ".vx(-1)", ".vx(-3)"};
    for (unsigned w = 0; w < WIDTHS; ++w) {
        const unsigned sew = widths[w];
        if (covered[w] != ELEMENTS)
            return reportFailure(sew, "vl", " total", 0, covered[w],
                                 ELEMENTS);
        for (size_t i = 0; i < ELEMENTS; ++i) {
            const uint64_t a = loadElement(inA[w], i, sew);
            const uint64_t b = loadElement(inB[w], i, sew);
            for (unsigned form = 0; form <= SCALARS; ++form) {
                uint64_t divisor = b;
                if (form > 0)
                    divisor = (uint64_t)scalars[form - 1] & maskOf(sew);
                for (unsigned op = 0; op < OPS; ++op) {
                    const uint64_t expected =
                        reference((enum Operation)op, a, divisor, sew);
                    const uint64_t got =
                        loadElement(out[w][OPS * form + op], i, sew);
                    if (got != expected)
                        return reportFailure(sew, operationNames[op],
                                             forms[form], i, got, expected);
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
    for (unsigned w = 0; w < WIDTHS; ++w) {
        for (unsigned o = 0; o < OUTPUTS; ++o) {
            for (size_t i = 0; i < ELEMENTS * (widths[w] / 8); ++i) {
                sum += (uint64_t)out[w][o][i] * (i % 251 + 1);
                xor_value ^= (uint32_t)out[w][o][i] << (8 * (i % 4));
            }
        }
    }
    printText("[muldiv] checksum: sum=");
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
    printText("[muldiv] PASS: SEW 8/16/32/64, ");
    printInt(ELEMENTS);
    printText(" elements, .vv and .vx with scalars 0, -1, -3\n");
    return 0;
}
