// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include "ip.h"
#include "net.h"

#include <stdbool.h>
#include <stdint.h>

/* DHCP, RFC 2131: how the machine is told an address to call its own.
 *
 * A whole frame goes in and a whole frame comes out.  The sequencing, which
 * reply is ours, and the transport -- both reserved ports, the broadcast a
 * client with no address must use, the UDP and IPv4 headers -- are all fixed by
 * the RFC, so a caller asked to supply them could supply only one answer.  The
 * clock is the exception, and stays the caller's.
 *
 * Frames are built and read, never sent, so this is tested on the host,
 * sequencing included. */

/* What a caller gives dhcp_step() to build into, and the most it returns.  A
 * BOOTP header runs 236 bytes before the first option, so the datagram is 300
 * and the frame this -- far memory, not the 16-bit window. */
constexpr uint16_t DHCP_FRAME_BYTES = UDP_PAYLOAD_AT + 300;

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

/* How far the exchange got.  More than dhcp_leased() gives: still discovering
 * means nothing was offered, still requesting means an offer nobody
 * acknowledged -- two different failures. */
enum DhcpStage : uint8_t {
    DhcpDiscovering,
    DhcpRequesting,
    DhcpLeased,
};

/* The exchange in progress.  A caller reads `stage` and `lease`; `xid` and
 * `mac` are how a reply is recognised, and are public only so a test can forge
 * one that will be. */
struct DhcpClient {
    uint8_t xid[4]; /* wire order, so no byte-splitting at either end */
    uint8_t mac[MAC_BYTES];
    enum DhcpStage stage;
    struct DhcpLease lease;
};

/* Begins an exchange for `mac`.
 *
 * The identifier a server echoes back is all that marks a reply as ours.  The
 * address separates this machine from its neighbours; `seed` separates this run
 * from the last, without which a late reply to the previous run reads as an
 * answer to this one.  Any value the caller has that moves will do. */
void dhcp_start(struct DhcpClient* client, const uint8_t* mac, uint16_t seed);

/* Cranks the exchange: returns the length of the frame left in `out`, or 0 when
 * there is nothing to send.  `in` is a received frame; `in_length` 0 means a
 * timeout, so say the current step again.  Anything not this exchange's reply
 * is dropped here, which is most of what arrives -- the receiver is promiscuous
 * and a DHCP reply is a broadcast that lands whoever it was for.
 *
 * The whole loop: start, step with nothing, send what comes back; then step
 * with every frame and on every timeout, sending anything non-zero, until
 * dhcp_leased(). */
[[nodiscard]] uint16_t dhcp_step(
    struct DhcpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out);

[[nodiscard]] static inline bool dhcp_leased(const struct DhcpClient* client) {
    return client->stage == DhcpLeased;
}
