#pragma once

/* Moving bytes across the 28-bit bus: DMAgic copies and fills, and the
 * byte-at-a-time load and store they replace once there is more than a byte to
 * move. */

#include "addr28.h"

#include <stdint.h>

void m65_io_enable(void);

/* A flat 28-bit load/store, where the DMA-based versions below cost a whole
 * DMA list per byte.  Ours rather than mega65-libc's, in flatmem.S, but the
 * signatures must stay libc's so its own callers still link. */
__attribute__((leaf)) uint8_t lpeek(uint32_t address);
__attribute__((leaf)) void lpoke(uint32_t address, uint8_t value);

void lcopy(Addr28 source_address, Addr28 destination_address, uint16_t count);
void lfill(Addr28 destination_address, uint8_t value, uint16_t count);

/* The same copy, for addresses the CPU can name directly: `_in` reads far to
 * near, `_out` writes near to far, `_near` is both.  Widening happens here
 * rather than at every call site, and must stay out of line or it folds back
 * into the caller and there is nothing left to save.  Pointers rather than
 * integers, so the compiler refuses a far address -- the mistake this invites.
 *
 * Worth it only where all twelve tools link the caller, which is why the
 * COMMON sources use these and the per-tool ones still cast: converting
 * filehost's own sites was measured at +198 bytes, not a saving. */
void lcopy_near(const void* from, void* to, uint16_t count);
void lcopy_out(const void* from, Addr28 to, uint16_t count);
void lcopy_in(Addr28 from, void* to, uint16_t count);

/* As above, but stepping the destination `skip` bytes at a time, which is what
 * writing one plane of a two-byte cell needs.  `count` still counts bytes
 * written, not bytes of destination spanned.  The DMAgic restores a step of one
 * at the end of every chain, so a strided job cannot leak into the next. */
void lcopy_skip(Addr28 source_address, Addr28 destination_address, uint16_t count, uint8_t skip);
void lfill_skip(Addr28 destination_address, uint8_t value, uint16_t count, uint8_t skip);

/* The SDK's PEEK/POKE are volatile, which hardware access requires. */
#include <peekpoke.h>
