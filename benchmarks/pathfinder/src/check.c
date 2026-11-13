/*
 * Pathfinder for the gem5 VPU, adapted from RiVEC.
 *
 * Copyright (c) 2008-2014 University of Virginia (Rodinia Pathfinder).
 * Copyright (c) 2020 Barcelona Supercomputing Center (RiVEC RISC-V
 * vectorized version, Cristóbal Ramírez Lazo).
 * Copyright (c) 2026 Félix Garcia Narocki (UCM) (freestanding adaptation).
 * All rights reserved.
 *
 * Redistribution and use are subject to the licenses in LICENSE-RODINIA
 * and LICENSE-RIVEC, which are BSD-style licenses that require keeping
 * these notices and disclaimers in source and binary redistributions.
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
 * Scalar reference and result checks of Pathfinder, shared by the gem5
 * and Ara RTL programs. Output goes through printText() and printInt(),
 * which each platform program implements.
 */

#include <stddef.h>
#include <stdint.h>

#include "pathfinder.h"

/* Two rows for the scalar version, which alternates source/destination. */
static int32_t scalarRows[2][PATHFINDER_COLS];

/*
 * Scalar version, as in RiVEC run(), used as an in-program reference.
 * Returns the row that holds the final result.
 */
const int32_t *
runScalar(void)
{
    const size_t cols = PATHFINDER_COLS;
    int32_t *src = scalarRows[1];
    int32_t *dst = scalarRows[0];

    for (size_t x = 0; x < cols; ++x)
        dst[x] = pathfinderWall[x];

    for (size_t t = 0; t < PATHFINDER_ROWS - 1; t++) {
        int32_t *temp = src;
        src = dst;
        dst = temp;
        for (size_t n = 0; n < cols; n++) {
            int32_t min = src[n];
            if (n > 0 && src[n - 1] < min)
                min = src[n - 1];
            if (n < cols - 1 && src[n + 1] < min)
                min = src[n + 1];
            dst[n] = pathfinderWall[(t + 1) * cols + n] + min;
        }
    }
    return dst;
}

/*
 * Compares every element against the reference from the input file.
 * Prints the first mismatch and returns 1 on failure, 0 on success.
 */
int
checkRow(const char *name, const int32_t *row)
{
    for (size_t i = 0; i < PATHFINDER_COLS; ++i) {
        if (row[i] != pathfinderReference[i]) {
            printText("[pathfinder] FAIL: ");
            printText(name);
            printText("[");
            printInt((long)i);
            printText("]=");
            printInt(row[i]);
            printText(", reference=");
            printInt(pathfinderReference[i]);
            printText("\n");
            return 1;
        }
    }
    return 0;
}

/*
 * Prints a fingerprint of the vector result so that runs in different
 * modes can be compared beyond PASS. Elements are taken as uint32_t: the
 * sum (at most COLS * 2^32) fits in 64 bits and the xor in 32 bits.
 */
void
printChecksum(const int32_t *row)
{
    uint64_t sum = 0;
    uint32_t xor_value = 0;
    for (size_t i = 0; i < PATHFINDER_COLS; ++i) {
        sum += (uint32_t)row[i];
        xor_value ^= (uint32_t)row[i];
    }
    printText("[pathfinder] checksum: sum=");
    printInt((long)sum);
    printText(" xor=");
    printInt((long)xor_value);
    printText("\n");
}

/* Prints the success line, e.g. "[pathfinder] PASS: 32x32, runs=1". */
void
printPass(void)
{
    printText("[pathfinder] PASS: ");
    printInt(PATHFINDER_ROWS);
    printText("x");
    printInt(PATHFINDER_COLS);
    printText(", runs=");
    printInt(NUM_RUNS);
    printText("\n");
}
