#include "infohelper.h"

/* Mirrors the assembler's mega65.inc; kept here so the inline asm below can
 * take them as immediates rather than embedding literals. */
#define HTRAP_DOS 0xD640
#define HYPPO_GETVERSION 0x00

/* buffer is written by the asm below, which the check cannot see into.
 * NOLINTNEXTLINE(readability-non-const-parameter) */
void hyppo_getversion(unsigned char* buffer) {
    /* Z is not expressible as a constraint or clobber, so the trailing ldz #0
     * restoring the Z==0 invariant compiled code depends on must be manual. */
    __asm__ volatile("lda #%c[fn]\n\t"
                     "sta %c[trap]\n\t"
                     "clv\n\t" /* CPU errata: hyper traps need a dead slot */
                     /* Version lands in A/X/Y/Z; Y must survive the A store. */
                     "phy\n\t"
                     "ldy #0\n\t"
                     "sta (%[buf]),y\n\t"
                     "iny\n\t"
                     "txa\n\t"
                     "sta (%[buf]),y\n\t"
                     "iny\n\t"
                     "pla\n\t"
                     "sta (%[buf]),y\n\t"
                     "iny\n\t"
                     "tza\n\t"
                     "sta (%[buf]),y\n\t"
                     "ldz #0"
        :
        : [fn] "i"(HYPPO_GETVERSION), [trap] "i"(HTRAP_DOS), [buf] "r"(buffer)
        : "a", "x", "y", "p", "memory");
}
