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
 * Pathfinder program for Ara RTL (Verilator), used to contrast the gem5
 * VPU with Ara. It links the same kernel.o, data.o and check.o as the gem5
 * program (../src/main.c) and only replaces what is platform specific:
 *
 * - Startup, output and exit come from Ara's bare-metal runtime
 *   (apps/common: crt0, printf over the fake UART, tohost).
 * - The region of interest is measured with the cycle CSR around the
 *   call to runVector(), instead of the gem5 m5ops. get_cycle_count()
 *   issues a fence first, so the end waits for the last vector store,
 *   as the m5 dump does in Minor.
 *
 * PATHFINDER_SKIP_SCALAR=1 skips the scalar reference (runScalar), which
 * takes several times the kernel in Ara, for traced runs. The vector
 * result is still checked against the reference of the input file.
 */

#include <stdint.h>

#include "pathfinder.h"
#include "printf.h"
#include "runtime.h"

#ifndef PATHFINDER_SKIP_SCALAR
#define PATHFINDER_SKIP_SCALAR 0
#endif

void
printText(const char *text)
{
    printf("%s", text);
}

void
printInt(long value)
{
    printf("%ld", value);
}

int
main(void)
{
    start_timer();
    runVector();
    stop_timer();
    const int64_t cycles = get_timer();

    printText("[pathfinder] kernel cycles: ");
    printInt((long)cycles);
    printText("\n");

    printChecksum(vectorResult);
    int failures = checkRow("vector", vectorResult);
#if !PATHFINDER_SKIP_SCALAR
    failures += checkRow("scalar", runScalar());
#endif
    if (failures != 0)
        return 1;
    printPass();
    return 0;
}
