// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include "net.h" /* NetEndpoint, IPV4_BYTES, the byte helpers */

#include <stdbool.h>
#include <stdint.h>

/* IPv4 and UDP: enough of RFC 791 and RFC 768 to carry DHCP, with the IPv4
 * half serving TCP beside it, and no more --
 * building a datagram, reading one, and the one question about an address that
 * has to be answered before either.
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
/* Where whatever IPv4 carries begins, options being refused rather than
 * followed. */
constexpr uint8_t IPV4_PAYLOAD_AT = ETH_HEADER_BYTES + IPV4_HEADER_BYTES;
/* The two protocol numbers this stack has adapters for, IANA's own. */
constexpr uint8_t IPV4_PROTOCOL_TCP = 6;
constexpr uint8_t IPV4_PROTOCOL_UDP = 17;
/* Ethernet, IPv4 and UDP headers together: where a payload begins, and where
 * it always begins -- a header carrying options is refused rather than
 * followed, so this is a constant rather than a property of what arrived. */
constexpr uint8_t UDP_PAYLOAD_AT = ETH_HEADER_BYTES + IPV4_HEADER_BYTES + UDP_HEADER_BYTES;

/* One's-complement sum, RFC 1071, carried between calls so a checksum can span
 * pieces that are not next to each other -- a UDP checksum covers a pseudo
 * header that is nowhere in the datagram.
 *
 * Only the final piece may have an odd length: an odd piece is padded to the
 * next word, which is only the same answer when nothing follows it.
 *
 * Public although udp_build() is the only caller: a wrong checksum is a
 * datagram discarded in silence at the far end, so the sum is worth checking
 * against RFC 1071's own worked example directly rather than only through a
 * datagram built with it. */
[[nodiscard]] uint32_t ip_sum(uint32_t sum, const uint8_t* data, uint16_t length);
/* Folds an accumulated sum into the sixteen bits a header carries. */
[[nodiscard]] uint16_t ip_sum_final(uint32_t sum);

/* The address in `text`, and the port after a colon if it names one: "10.0.0.1"
 * or "10.0.0.1:6969".  False when it is neither, and then both outputs are left
 * alone -- half an address is worse than the one already in use.
 *
 * `port` is untouched where no port is given, so a caller's own default stands
 * without having to be spelled twice.  Whatever ends the line need not be
 * removed first: a terminator, a space or either newline character ends the
 * text, and anything else after it means it is not an address. */
[[nodiscard]] bool ip_parse(const char* text, uint8_t* out, uint16_t* port);

/* Which address to put on the wire to reach `target`: `target` itself when it
 * shares this machine's network, and `router` when it does not.
 *
 * Two addresses are in play whenever a datagram leaves the subnet -- the one it
 * is addressed to and the one whose hardware address carries it -- and asking
 * ARP for the wrong one gets no answer at all.  Returned as the address to ask
 * for, rather than as a yes or no, so a caller cannot take the answer the wrong
 * way round. */
[[nodiscard]] const uint8_t* ip_next_hop(
    const uint8_t* target, const uint8_t* us, const uint8_t* netmask, const uint8_t* router);

/* The ethernet and IPv4 headers of a datagram carrying `payload_length` bytes
 * of `protocol`, checksum included -- everything below the transport, and
 * nothing of it.  The caller fills IPV4_PAYLOAD_AT onwards.
 *
 * Public because there are two adapters above it, UDP and TCP, and one copy of
 * the checksum, the don't-fragment reasoning and the TTL is the point.  While
 * there was one adapter this was rightly inlined in udp_build(). */
void ipv4_build_header(uint8_t* frame,
    const struct NetEndpoint* from,
    const struct NetEndpoint* to,
    uint8_t protocol,
    uint16_t payload_length);

/* What arrived, and who from.
 *
 * One struct for both layers rather than one per layer: IPv4 answers every
 * field of it except the port, and the transport above fills that in where it
 * has one.  Two identical structs and a copy between them measured 61 bytes
 * for an answer that cannot differ. */
struct Datagram {
    /* Who sent it, in the shape a reply to them needs.  `port` is left alone
     * by ipv4_parse(): IPv4 has no notion of one. */
    struct NetEndpoint from;
    /* What the header says follows it, checked against what the frame holds --
     * so a caller may walk this many bytes from IPV4_PAYLOAD_AT. */
    uint16_t payload_length;
};

/* True when `frame` is an IPv4 datagram of `protocol` for `us`, and then `out`
 * describes it.  An all-zero `us->ip` accepts any destination, which is what a
 * machine that has not been given an address yet must do to hear a DHCP offer.
 *
 * Refuses a header carrying options and a datagram in fragments: options would
 * move every offset above, and half a datagram treated as a whole one is worse
 * than a lost one. */
[[nodiscard]] bool ipv4_parse(const uint8_t* frame,
    uint16_t length,
    const struct NetEndpoint* us,
    uint8_t protocol,
    struct Datagram* out);

/* The twelve bytes RFC 768 and RFC 793 both have a transport checksum cover
 * but neither transmits: the addresses, the protocol and the transport length
 * again.  `payload_length` counts the transport header and its data. */
[[nodiscard]] uint32_t ipv4_pseudo_sum(
    const uint8_t* source, const uint8_t* destination, uint8_t protocol, uint16_t payload_length);

/* Fills `frame` with an ethernet/IPv4/UDP datagram carrying `payload`, and
 * returns the whole frame's length.  The caller owns at least
 * UDP_PAYLOAD_AT + payload_length bytes. */
[[nodiscard]] uint16_t udp_build(uint8_t* frame,
    const struct NetEndpoint* from,
    const struct NetEndpoint* to,
    const uint8_t* payload,
    uint16_t payload_length);

/* True when `frame` is a UDP datagram for `us`, and then `out` describes it;
 * its payload is at UDP_PAYLOAD_AT.  An all-zero `us->ip` accepts any
 * destination, which is what a machine that has not been given an address yet
 * must do to hear a DHCP offer.
 *
 * A wrong UDP checksum is a rejection; a zero one is not, since RFC 768 makes
 * it optional over IPv4 and says zero means it was not computed. */
[[nodiscard]] bool udp_parse(
    const uint8_t* frame, uint16_t length, const struct NetEndpoint* us, struct Datagram* out);
