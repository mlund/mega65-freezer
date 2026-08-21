#pragma once

/* Moving bytes across the 28-bit bus: DMAgic copies and fills, and the
 * byte-at-a-time load and store they replace once there is more than a byte to
 * move. */

#include "addr28.h"

#include <stdint.h>

void m65_io_enable(void);

/* From mega65-libc: a flat 28-bit load/store, where the DMA-based versions
 * below cost a whole DMA list per byte.  Signatures must match libc's. */
__attribute__((leaf)) uint8_t lpeek(uint32_t address);
__attribute__((leaf)) void lpoke(uint32_t address, uint8_t value);

void lcopy(Addr28 source_address, Addr28 destination_address, uint16_t count);

/* The same copy and fill, for addresses the CPU can name directly.
 *
 * Two of an Addr28's four bytes are always zero below $10000, and this ABI
 * carries two argument bytes free and charges about four bytes of code for
 * each one after them, at every call site.  Widening a near pointer in here
 * rather than out there is eight bytes per site for one such address and
 * sixteen for two, against one body each.
 *
 * Out of line for that reason: inlined, the widening folds back into the
 * caller and there is nothing left to save.  And a pointer rather than an
 * integer deliberately -- the compiler then refuses a far address here, which
 * is the one mistake this shape invites. */
__attribute__((noinline)) void lcopy_near(const void* from, void* to, uint16_t count);
__attribute__((noinline)) void lcopy_out(const void* from, Addr28 to, uint16_t count);
__attribute__((noinline)) void lcopy_in(Addr28 from, void* to, uint16_t count);
__attribute__((noinline)) void lfill_near(void* at, uint8_t value, uint16_t count);
void lfill(Addr28 destination_address, unsigned char value, uint16_t count);

/* As above, but stepping the destination `skip` bytes at a time, which is what
 * writing one plane of a two-byte cell needs.  `count` still counts bytes
 * written, not bytes of destination spanned.  The DMAgic restores a step of one
 * at the end of every chain, so a strided job cannot leak into the next. */
void lcopy_skip(Addr28 source_address, Addr28 destination_address, uint16_t count, uint8_t skip);
void lfill_skip(Addr28 destination_address, uint8_t value, uint16_t count, uint8_t skip);

/* The SDK's PEEK/POKE are volatile, which hardware access requires. */
#include <peekpoke.h>
