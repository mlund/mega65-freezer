// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include "eth.h" /* MAC_BYTES */

#include <stdbool.h>
#include <stdint.h>

/* What every protocol above ethernet needs and none of them owns.
 *
 * The wire is big-endian and this CPU is not, so the two-byte fields are
 * assembled a byte at a time rather than through a cast -- a cast would read
 * correctly on the host that tests this and backwards on the machine that runs
 * it.  These are inline because at -Oz the compiler emits them inline anyway;
 * a header costs nothing over a copy per translation unit. */

constexpr uint8_t IPV4_BYTES = 4;

/* Stores a 16-bit field in wire order. */
static inline void net_put16(uint8_t* at, uint16_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

/* Reads one back. */
[[nodiscard]] static inline uint16_t net_get16(const uint8_t* at) {
    return (uint16_t)(((uint16_t)at[0] << 8) | at[1]);
}

/* The same, four bytes wide, for the sequence numbers RFC 793 counts a
 * connection with -- the one field on this wire that does not fit sixteen
 * bits. */
static inline void net_put32(uint8_t* at, uint32_t value) {
    net_put16(at, (uint16_t)(value >> 16));
    net_put16(at + 2, (uint16_t)value);
}

/* Reads one back. */
[[nodiscard]] static inline uint32_t net_get32(const uint8_t* at) {
    return ((uint32_t)net_get16(at) << 16) | net_get16(at + 2);
}

/* Whether an address is all zeros, which across this stack means "not yet
 * given one": nothing claims such an address, answers for it, or matches
 * against it. */
[[nodiscard]] static inline bool net_zero(const uint8_t* a, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (a[i]) {
            return false;
        }
    }
    return true;
}

/* Whether two addresses -- a MAC, an IP -- are the same one. */
[[nodiscard]] static inline bool net_same(const uint8_t* a, const uint8_t* b, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* Who a datagram is from or to.  Grouped because the three travel together and
 * because the alternative is nine arguments: this ABI carries two bytes free
 * and charges about four bytes of code per byte after that, at every call
 * site, so a flat signature re-marshals eight pointers per datagram where this
 * passes one. */
struct NetEndpoint {
    uint8_t mac[MAC_BYTES];
    uint8_t ip[IPV4_BYTES];
    uint16_t port;
};
