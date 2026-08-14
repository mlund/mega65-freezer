// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "net.h"

#include <stdbool.h>
#include <stdint.h>

/* DHCP, RFC 2131: how the machine is told an address to call its own.
 *
 * Building and reading the messages is separate from sending them, so this
 * compiles for the host and is tested there.  The exchange is
 * discover -> offer -> request -> acknowledge, and the client half of it is
 * four function calls with the caller owning the waiting and retrying.
 *
 * The messages carry a BOOTP header of 236 bytes before a single option is
 * reached, which is why a datagram runs to 300 and its buffer belongs in far
 * memory rather than in the 16-bit window. */

constexpr uint16_t DHCP_CLIENT_PORT = 68;
constexpr uint16_t DHCP_SERVER_PORT = 67;
/* What RFC 2131 asks a client to be able to send and accept.  Also what is
 * built: the message is padded to it. */
constexpr uint16_t DHCP_PAYLOAD_BYTES = 300;

enum DhcpMessage : uint8_t {
    DhcpNothing = 0,
    DhcpOffer = 2,
    DhcpAck = 5,
    DhcpNak = 6,
};

/* What a server told us.  `tftp` is the point of asking: options 150 and 66
 * are how a DHCP server names a TFTP server, which is the standard answer to
 * "where do I fetch from" and costs nothing once a lease is being taken
 * anyway.  A server that says nothing leaves `has_tftp` false, and finding the
 * gateway is then somebody else's problem. */
struct DhcpLease {
    uint8_t ip[IPV4_BYTES];
    uint8_t server[IPV4_BYTES];
    uint8_t router[IPV4_BYTES];
    uint8_t dns[IPV4_BYTES];
    uint8_t netmask[IPV4_BYTES];
    uint8_t tftp[IPV4_BYTES];
    bool has_tftp;
};

/* Fills `payload` with a DHCPDISCOVER and returns its length.  The caller owns
 * DHCP_PAYLOAD_BYTES. */
uint16_t dhcp_discover(uint8_t* payload, const uint8_t* mac, uint32_t xid);

/* The same for the DHCPREQUEST that accepts `offer`. */
uint16_t dhcp_request(
    uint8_t* payload, const uint8_t* mac, uint32_t xid, const struct DhcpLease* offer);

/* What `payload` is, and what it said.  Anything that is not a reply to this
 * machine's own exchange -- another client's, or another run of ours -- reads
 * as DhcpNothing, because a LAN carries other people's DHCP traffic and a
 * broadcast reply arrives whether or not it was meant for us. */
enum DhcpMessage dhcp_parse(const uint8_t* payload,
    uint16_t length,
    uint32_t xid,
    const uint8_t* mac,
    struct DhcpLease* out);
