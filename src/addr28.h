#pragma once

#include <stdint.h>

/* The 45GS02 address bus is 28 bits wide, hence the name.  C23's
 * `unsigned _BitInt(28)` would state that in the type system, and does compile
 * as of clang 23 (clang 22 accepted it but failed at LTO link time, "unable to
 * legalize G_MERGE_VALUES").  It is still not worth it: the exact width forces
 * a conversion at every call site, costing 582 bytes.  `uint32_t` would match
 * mega65-libc but costs 694 for a sign bit these addresses never reach.
 *
 * `int32_t` rather than plain `long` for the sake of the host tests.  On the
 * target the two are the same type, so the code generated is unchanged; on a
 * host `long` is 64 bits, and an address that overflows 28 or 32 bits there
 * quietly does not, so a test would pass on arithmetic the machine gets wrong.
 *
 * Alone in a header of its own so that a module with no hardware in it can name
 * the type and still compile for the host: dma.h reaches <peekpoke.h>. */
typedef int32_t Addr28;

/* The point of the spelling above, stated where it will be checked.  A host
 * build whose Addr28 is wider than the machine's silently stops overflowing
 * where the machine overflows, and the test that would have caught it passes. */
static_assert(sizeof(Addr28) == 4, "Addr28 must be 32 bits on host and target alike");
