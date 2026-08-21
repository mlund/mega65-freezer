#pragma once

#include <stdint.h>

/* The 45GS02 address bus is 28 bits wide, hence the name.  C23's
 * `unsigned _BitInt(28)` states that in the type system and compiles, but
 * forces a conversion at every call site: 582 bytes.  `uint32_t` would match
 * mega65-libc and costs 694, for a sign bit these addresses never reach.
 *
 * `int32_t` rather than plain `long` for the host tests: on the target they
 * are the same type, but a host `long` is 64 bits, so an address that should
 * overflow quietly would not and a test would pass on arithmetic the machine
 * gets wrong.
 *
 * Alone in a header so a module with no hardware can use it host side. */
typedef int32_t Addr28;

/* The point of the spelling above, stated where it will be checked.  A host
 * build whose Addr28 is wider than the machine's silently stops overflowing
 * where the machine overflows, and the test that would have caught it passes. */
static_assert(sizeof(Addr28) == 4, "Addr28 must be 32 bits on host and target alike");
