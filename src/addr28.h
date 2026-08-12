#pragma once

/* The 45GS02 address bus is 28 bits wide, hence the name.  C23's
 * `unsigned _BitInt(28)` would state that in the type system, and does compile
 * as of clang 23 (clang 22 accepted it but failed at LTO link time, "unable to
 * legalize G_MERGE_VALUES").  It is still not worth it: the exact width forces
 * a conversion at every call site, costing 582 bytes.  `uint32_t` would match
 * mega65-libc but costs 694 for a sign bit these addresses never reach.
 *
 * Alone in a header of its own so that a module with no hardware in it can name
 * the type and still compile for the host: dma.h reaches <peekpoke.h>. */
typedef long Addr28;
