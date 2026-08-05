#include "version.h"

/* The loader enters at a fixed address that precedes _start, so keep a real
 * jump there whatever the version string happens to spell. */
__asm__(".section .entrytramp,\"axR\",@progbits\n"
        "jmp _start\n");

/* Found by scanning memory rather than by linking, so it needs used/retain to
 * survive --gc-sections. */
__attribute__((used, retain, section(".version"))) const char VERSION[] = "V:" BUILD_VERSION;
