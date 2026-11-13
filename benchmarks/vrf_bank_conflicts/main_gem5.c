/*
 * Programa de gem5 SE para medir los conflictos de bancos del VRF con el
 * kernel original de Ara run_vector_short_m4 (Pathfinder, 7 x 512, int32,
 * LMUL=4). Enlaza los objetos que compiló Ara (kernel/pathfinder.c.o y
 * data.S.o de ara/apps/pathfinder_r7_c512), así que el kernel y los datos
 * son los mismos bytes que se simularon en Ara RTL. Ver README.md.
 */

#include <stddef.h>
#include <stdint.h>

extern int32_t num_runs, rows, cols;
extern int wall[], result_ref[], result_v[];
void run_vector_short_m4(int *wall, int *result_v, uint32_t cols,
                         uint32_t rows, uint32_t num_runs, int neutral_value);

/* m5ops reset (0x40) y dump (0x41) de estadísticas, como en pathfinder. */
#define M5OP(word)                                                        \
    do {                                                                  \
        register long a0 __asm__("a0") = 0;                               \
        register long a1 __asm__("a1") = 0;                               \
        __asm__ volatile(".long " word : "+r"(a0) : "r"(a1) : "memory"); \
    } while (0)

__asm__(".section .text\n"
        ".global _start\n"
        "_start:\n"
        ".option push\n.option norelax\n"
        "la gp, __global_pointer$\n"
        ".option pop\n"
        "call main\n"
        "li a7, 93\n"
        "ecall\n"
        "1: j 1b\n");

static void
writeText(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    register long a0 __asm__("a0") = 1;
    register const char *a1 __asm__("a1") = s;
    register size_t a2 __asm__("a2") = n;
    register long a7 __asm__("a7") = 64;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
}

static void
writeInt(long v)
{
    char d[24];
    int p = 23;
    d[p] = 0;
    if (v == 0)
        d[--p] = '0';
    while (v > 0) {
        d[--p] = (char)('0' + v % 10);
        v /= 10;
    }
    writeText(&d[p]);
}

int
main(void)
{
    M5OP("0x8000007b");
    run_vector_short_m4(wall, result_v, cols, rows, num_runs, 0x7fffffff);
    M5OP("0x8200007b");
    int ok = 0;
    for (int i = 0; i < cols; i++)
        ok += result_v[i] == result_ref[i];
    writeInt(ok);
    writeText("/");
    writeInt(cols);
    writeText(" resultados correctos\n");
    return ok != cols;
}
