#pragma once

#include <stdint.h>

void m65_io_enable(void);

/* From mega65-libc: a flat 28-bit load/store, where the DMA-based versions
 * below cost a whole DMA list per byte.  Signatures must match libc's. */
__attribute__((leaf)) uint8_t lpeek(uint32_t address);
__attribute__((leaf)) void lpoke(uint32_t address, uint8_t value);
/* The 45GS02 address bus is 28 bits wide, hence the name.  C23's
 * `unsigned _BitInt(28)` would state that in the type system, and does compile
 * as of clang 23 (clang 22 accepted it but failed at LTO link time, "unable to
 * legalize G_MERGE_VALUES").  It is still not worth it: the exact width forces
 * a conversion at every call site, costing 582 bytes.  `uint32_t` would match
 * mega65-libc but costs 694 for a sign bit these addresses never reach. */
typedef long Addr28;

void lcopy(Addr28 source_address, Addr28 destination_address, unsigned int count);
void lfill(Addr28 destination_address, unsigned char value, unsigned int count);

/* The SDK's PEEK/POKE are volatile, which hardware access requires. */
#include <peekpoke.h>
