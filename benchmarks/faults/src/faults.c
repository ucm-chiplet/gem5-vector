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
 * Page fault in the middle of a unit-stride vector access (criterion 8
 * of hito 2), SEW 32, LMUL m1.
 *
 * The program maps one anonymous page at the fixed address PAGE_BASE
 * with the mmap system call; the next page stays unmapped. A vle32.v
 * (VARIANT_LOAD) or vse32.v (VARIANT_STORE) with vl = 8 starts INSIDE
 * elements before the end of the page, so elements 0..INSIDE-1 are
 * mapped and element INSIDE is the first one in the unmapped page.
 *
 * RVV 1.0 (edition 20240411), section "Vector Memory Instructions" and
 * "Synchronous exception behavior": the trap is taken on the first
 * faulting element and vstart holds its index. Here the fault address
 * must be PAGE_BASE + 4096 and vstart must be INSIDE.
 *
 * In SE mode gem5 has no trap handler: invoking the page fault ends the
 * simulation with a panic. run.py treats that panic, with the expected
 * address and the vstart traced by Minor, as the expected result. If the
 * access does not fault, the program prints UNEXPECTED and returns 1.
 */

#include "vpu_runtime.h"

#define PAGE_BASE 0x40000000L
#define PAGE_SIZE 4096L
#define ELEMENTS_PER_PAGE (PAGE_SIZE / 4)
#ifndef INSIDE
#define INSIDE 4
#endif
#define VL 8

#if !defined(VARIANT_LOAD) && !defined(VARIANT_STORE)
#error "Define VARIANT_LOAD or VARIANT_STORE"
#endif

/* Linux mmap(addr, PAGE_SIZE, PROT_READ | PROT_WRITE,
 * MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0). */
static long
mapPage(long address)
{
    register long a0 __asm__("a0") = address;
    register long a1 __asm__("a1") = PAGE_SIZE;
    register long a2 __asm__("a2") = 0x3;
    register long a3 __asm__("a3") = 0x32;
    register long a4 __asm__("a4") = -1;
    register long a5 __asm__("a5") = 0;
    register long a7 __asm__("a7") = 222;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
    return a0;
}

#ifdef VARIANT_STORE
/* Data to store. Static initializer: an initialization loop would be
 * lowered to RVV outside runKernel(), which check_profile.py rejects. */
static int32_t source[VL] __attribute__((aligned(64))) = {
    1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007};
#endif

/* The faulting access, alone in runKernel() as check_profile.py
 * requires. */
static __attribute__((noinline)) void
runKernel(int32_t *address)
{
    size_t vl;
#ifdef VARIANT_LOAD
    __asm__ volatile("vsetvli %[vl], %[avl], e32, m1, ta, ma\n"
                     "vle32.v v8, (%[p])\n"
                     : [vl] "=&r"(vl)
                     : [avl] "r"((long)VL), [p] "r"(address)
                     : "v8", "memory");
#else
    __asm__ volatile("vsetvli %[vl], %[avl], e32, m1, ta, ma\n"
                     "vle32.v v8, (%[s])\n"
                     "vse32.v v8, (%[p])\n"
                     : [vl] "=&r"(vl)
                     : [avl] "r"((long)VL), [s] "r"(source),
                       [p] "r"(address)
                     : "v8", "memory");
#endif
    (void)vl;
}

int
main(void)
{
    if (mapPage(PAGE_BASE) != PAGE_BASE) {
        printText("[faults] FAIL: mmap did not map the page\n");
        return 1;
    }
    int32_t *page = (int32_t *)PAGE_BASE;
    for (long i = 0; i < ELEMENTS_PER_PAGE; ++i)
        page[i] = (int32_t)i;

    printText("[faults] armed: vl=8, fault expected at element ");
    printInt(INSIDE);
    printText("\n");
    /* The fault ends the simulation, so vpuRoiEnd() is only reached
     * when the access does not fault. */
    vpuRoiBegin();
    runKernel(page + ELEMENTS_PER_PAGE - INSIDE);
    vpuRoiEnd();
    printText("[faults] UNEXPECTED: the access did not fault\n");
    return 1;
}
