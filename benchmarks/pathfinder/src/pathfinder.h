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
 * Declarations shared by the Pathfinder kernel (kernel.c), the input data
 * (data.c), the scalar reference and checks (check.c) and the programs
 * of each platform (main.c for gem5, ../ara/main_ara.c for Ara RTL).
 */

#ifndef PATHFINDER_H
#define PATHFINDER_H

#include <stdint.h>

#include "pathfinder_input.h"

#ifndef NUM_RUNS
#define NUM_RUNS 1
#endif

/* kernel.c */
extern int32_t vectorResult[PATHFINDER_COLS];
void runVector(void);

/* check.c */
const int32_t *runScalar(void);
int checkRow(const char *name, const int32_t *row);
void printChecksum(const int32_t *row);
void printPass(void);

/* Output services, implemented by each platform program. */
void printText(const char *text);
void printInt(long value);

#endif
