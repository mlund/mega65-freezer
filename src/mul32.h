#pragma once

/* A 32-bit multiply that uses the MEGA65's multiplier where there is one.
 *
 * layout.c and catalog.c are compiled for the host as well, where mega65.h
 * does not exist, so they cannot reach mega65_regs.h directly. */

#include <stdint.h>

#if defined(__mos__)
#include "mega65_regs.h"
static inline uint32_t mul32(uint32_t a, uint32_t b) {
    return hw_mul32(a, b);
}
#else
static inline uint32_t mul32(uint32_t a, uint32_t b) {
    return a * b;
}
#endif
