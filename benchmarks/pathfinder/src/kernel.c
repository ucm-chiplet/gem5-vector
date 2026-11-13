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
 * Pathfinder vector kernel, shared by the gem5 program (main.c) and the
 * Ara RTL program (../ara/main_ara.c). Origin: RiVEC, revision
 * 32a14ddfb9acaeb677800e8d2e18a45d9421d4cc, _pathfinder/src/pathfinder.cpp,
 * run_vector(). The body keeps the original intrinsics, their order and
 * the in-place update of dst.
 *
 * It is a translation unit of its own so that a single object, kernel.o,
 * is linked into both programs: gem5 and Ara then execute the same kernel
 * instructions (checked by tools/compare_kernel.py). It must not depend
 * on anything specific to either platform.
 */

#include <riscv_vector.h>
#include <stddef.h>
#include <stdint.h>

#include "pathfinder.h"

/* Last row computed by the vector kernel; it is updated in place. */
int32_t vectorResult[PATHFINDER_COLS] PATHFINDER_ALIGNED;

/*
 * Vector kernel, as in RiVEC run_vector(). For each row t+1, each element
 * takes the minimum of itself and its two neighbours in row t and adds
 * the wall cost: dst[n] = wall[t+1][n] + min(dst[n-1], dst[n], dst[n+1]).
 *
 * dst is updated in place, one strip of gvl elements at a time. The
 * neighbours across strip boundaries come from scalar reads: aux keeps
 * the last element of the previous strip before it is overwritten, and
 * aux2 reads the first element of the next strip, still unmodified. At
 * the row ends the element itself is used, which leaves the minimum
 * unchanged. Those scalar reads after vector stores exercise the CPU/VPU
 * memory ordering.
 *
 * The initial copy is scalar: -fno-vectorize keeps it out of RVV.
 * noinline keeps the kernel in its own symbol, so check_profile.py
 * can verify that every RVV instruction belongs to it.
 */
__attribute__((noinline)) void
runVector(void)
{
    const size_t cols = PATHFINDER_COLS;
    const size_t rows = PATHFINDER_ROWS;
    int32_t *dst = vectorResult;

    for (int run = 0; run < NUM_RUNS; ++run) {
        for (size_t x = 0; x < cols; ++x)
            vectorResult[x] = pathfinderWall[x];
        dst = vectorResult;

        size_t gvl = __riscv_vsetvl_e32m1(cols);

        vint32m1_t xSrc_slideup;
        vint32m1_t xSrc_slidedown;
        vint32m1_t xSrc;
        vint32m1_t xNextrow;

        int32_t aux;
        int32_t aux2;

        for (size_t t = 0; t < rows - 1; t++) {
            aux = dst[0];
            for (size_t n = 0; n < cols; n = n + gvl) {
                gvl = __riscv_vsetvl_e32m1(cols - n);
                xNextrow = __riscv_vle32_v_i32m1(&dst[n], gvl);
                xSrc = xNextrow;
                aux2 = (n + gvl >= cols) ? dst[n + gvl - 1] : dst[n + gvl];
                xSrc_slideup = __riscv_vslide1up_vx_i32m1(xSrc, aux, gvl);
                xSrc_slidedown =
                    __riscv_vslide1down_vx_i32m1(xSrc, aux2, gvl);
                xSrc = __riscv_vmin_vv_i32m1(xSrc, xSrc_slideup, gvl);
                xSrc = __riscv_vmin_vv_i32m1(xSrc, xSrc_slidedown, gvl);
                xNextrow = __riscv_vle32_v_i32m1(
                    &pathfinderWall[(t + 1) * cols + n], gvl);
                xNextrow = __riscv_vadd_vv_i32m1(xNextrow, xSrc, gvl);
                aux = dst[n + gvl - 1];
                __riscv_vse32_v_i32m1(&dst[n], xNextrow, gvl);
            }
        }
    }
}
