// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "net.h" /* NetEndpoint, IPV4_BYTES, the byte helpers */

#include <stdbool.h>
#include <stdint.h>

/* IPv4 and UDP: enough of RFC 791 and RFC 768 to carry TFTP, and no more.
 *
 * Building and reading datagrams is separate from putting them on the wire
 * (eth.h), so this compiles for the host and is tested there.  That matters
 * more here than elsewhere: a wrong checksum is not an error anybody reports,
 * it is a datagram the far end discards in silence.
 *
 * No fragmentation, in either direction.  Nothing this sends approaches the
 * 1500-byte limit, so what it sends is marked don't-fragment; a fragment that
 * arrives is dropped rather than reassembled, since half a datagram treated as
 * a whole one is worse than a lost one. */

constexpr uint8_t IPV4_HEADER_BYTES = 20;
constexpr uint8_t UDP_HEADER_BYTES = 8;
/* Ethernet, IPv4 and UDP headers together: where a payload begins, and where
 * it always begins -- a header carrying options is refused rather than
 * followed, so this is a constant rather than a property of what arrived. */
constexpr uint8_t UDP_PAYLOAD_AT = ETH_HEADER_BYTES + IPV4_HEADER_BYTES + UDP_HEADER_BYTES;

/* One's-complement sum, RFC 1071, carried between calls so a checksum can span
 * pieces that are not next to each other -- a UDP checksum covers a pseudo
 * header that is nowhere in the datagram.
 *
 * Only the final piece may have an odd length: an odd piece is padded to the
 * next word, which is only the same answer when nothing follows it. */
[[nodiscard]] uint32_t ip_sum(uint32_t sum, const uint8_t* data, uint16_t length);
/* Folds an accumulated sum into the sixteen bits a header carries. */
[[nodiscard]] uint16_t ip_sum_final(uint32_t sum);

/* Fills `frame` with an ethernet/IPv4/UDP datagram carrying `payload`, and
 * returns the whole frame's length.  The caller owns at least
 * UDP_PAYLOAD_AT + payload_length bytes. */
[[nodiscard]] uint16_t udp_build(uint8_t* frame,
    const struct NetEndpoint* from,
    const struct NetEndpoint* to,
    const uint8_t* payload,
    uint16_t payload_length);

struct UdpDatagram {
    /* Who sent it, in the shape a reply to them needs. */
    struct NetEndpoint from;
    uint16_t payload_length;
};

/* True when `frame` is a UDP datagram for `us`, and then `out` describes it;
 * its payload is at UDP_PAYLOAD_AT.  An all-zero `us->ip` accepts any
 * destination, which is what a machine that has not been given an address yet
 * must do to hear a DHCP offer.
 *
 * A wrong UDP checksum is a rejection; a zero one is not, since RFC 768 makes
 * it optional over IPv4 and says zero means it was not computed. */
[[nodiscard]] bool udp_parse(
    const uint8_t* frame, uint16_t length, const struct NetEndpoint* us, struct UdpDatagram* out);
