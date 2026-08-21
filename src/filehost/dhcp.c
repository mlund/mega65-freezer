// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* The BOOTP frame DHCP rides on, the options this client speaks, and the
 * sequencing that turns four messages into a lease. */

#include "dhcp.h"

#include <stddef.h>
#include <string.h>

/* The BOOTP header RFC 2131 builds on, by offset. */
static constexpr uint8_t BOOTP_OP = 0;
static constexpr uint8_t BOOTP_HTYPE = 1;
static constexpr uint8_t BOOTP_HLEN = 2;
static constexpr uint8_t BOOTP_XID = 4;
static constexpr uint8_t BOOTP_FLAGS = 10;
static constexpr uint8_t BOOTP_YIADDR = 16;
static constexpr uint8_t BOOTP_CHADDR = 28;
static constexpr uint16_t BOOTP_COOKIE = 236;
static constexpr uint16_t DHCP_OPTIONS_AT = 240;

static constexpr uint8_t BOOTREQUEST = 1;
static constexpr uint8_t BOOTREPLY = 2;
static constexpr uint8_t HTYPE_ETHERNET = 1;
/* Ask for the reply as a broadcast: until the lease is taken there is no
 * address for a server to unicast to, and not every server will answer one
 * that is not yet configured. */
static constexpr uint16_t BOOTP_BROADCAST = 0x8000;
static const uint8_t MAGIC_COOKIE[4] = {99, 130, 83, 99};

/* The options this speaks, by number (RFC 2132, and RFC 5859 for 150). */
static constexpr uint8_t OPTION_NETMASK = 1;
static constexpr uint8_t OPTION_ROUTER = 3;
static constexpr uint8_t OPTION_DNS = 6;
static constexpr uint8_t OPTION_REQUESTED_IP = 50;
static constexpr uint8_t OPTION_MESSAGE_TYPE = 53;
static constexpr uint8_t OPTION_SERVER_ID = 54;
static constexpr uint8_t OPTION_PARAMETER_LIST = 55;
static constexpr uint8_t OPTION_PAD = 0;
static constexpr uint8_t OPTION_END = 255;

static constexpr uint8_t DHCPDISCOVER = 1;
static constexpr uint8_t DHCPREQUEST = 3;

/* The two reserved ports, and the payload inside DHCP_FRAME_BYTES. */
static constexpr uint16_t DHCP_CLIENT_PORT = 68;
static constexpr uint16_t DHCP_SERVER_PORT = 67;
static constexpr uint16_t DHCP_PAYLOAD_BYTES = DHCP_FRAME_BYTES - UDP_PAYLOAD_AT;

/* What a reply turned out to be.  Internal: a caller is told the stage it
 * reached, not the message that got it there. */
enum DhcpMessage : uint8_t {
    DhcpNothing = 0,
    DhcpOffer = 2,
    DhcpAck = 5,
    DhcpNak = 6,
};

/* The options that carry one address and differ only in where it belongs.  A
 * table rather than five near-identical cases: each case re-derived the source
 * pointer, and five of them came to two fifths of the parser.
 *
 * The requested ones come first, so the parameter list is a prefix of this
 * table rather than a second copy of it.  Asking for an option the walk cannot
 * store, and storing one never asked for, are both silent.
 *
 * Option 6 is the address of a resolver, which is the thing that would let a
 * proxy be named rather than addressed.  Nothing reads it yet. */
static constexpr uint8_t ADDRESS_OPTIONS = 4;
static constexpr uint8_t REQUESTED_OPTIONS = 3;
static const uint8_t ADDRESS_OPTION[ADDRESS_OPTIONS] = {OPTION_NETMASK,
    OPTION_ROUTER,
    OPTION_DNS,
    /* Always volunteered, never requested. */
    OPTION_SERVER_ID};
static const uint8_t ADDRESS_FIELD[ADDRESS_OPTIONS] = {
    offsetof(struct DhcpLease, netmask),
    offsetof(struct DhcpLease, router),
    offsetof(struct DhcpLease, dns),
    offsetof(struct DhcpLease, server),
};

/* Appends one option -- number, length, value -- and says where the next one
 * goes. */
static uint8_t* put_option(uint8_t* at, uint8_t option, const uint8_t* value, uint8_t length) {
    *at++ = option;
    *at++ = length;
    memcpy(at, value, length);
    return at + length;
}

/* Everything the two messages share: a request from this machine, for this
 * exchange, saying who it is and what it would like to be told. */
static uint8_t* build(uint8_t* payload, const struct DhcpClient* client, uint8_t type) {
    memset(payload, 0, DHCP_PAYLOAD_BYTES);

    payload[BOOTP_OP] = BOOTREQUEST;
    payload[BOOTP_HTYPE] = HTYPE_ETHERNET;
    payload[BOOTP_HLEN] = MAC_BYTES;
    /* The exchange identifier is echoed back, and is how a reply is known to
     * be ours.  Held in wire order, so it is copied rather than taken apart. */
    memcpy(&payload[BOOTP_XID], client->xid, sizeof client->xid);
    net_put16(&payload[BOOTP_FLAGS], BOOTP_BROADCAST);
    net_copy(&payload[BOOTP_CHADDR], client->mac, MAC_BYTES);
    memcpy(&payload[BOOTP_COOKIE], MAGIC_COOKIE, sizeof MAGIC_COOKIE);

    uint8_t* at = &payload[DHCP_OPTIONS_AT];
    at = put_option(at, OPTION_MESSAGE_TYPE, &type, 1);
    return put_option(at, OPTION_PARAMETER_LIST, ADDRESS_OPTION, REQUESTED_OPTIONS);
}

/* Whichever message the current stage owes the server.  The length is always
 * the full one: build() zeroed the buffer, so everything past the end option is
 * already the pad option, and RFC 2131 asks a client to send at least this
 * much. */
static uint16_t say(const struct DhcpClient* client, uint8_t* payload) {
    const bool requesting = client->stage == DhcpRequesting;
    uint8_t* at = build(payload, client, requesting ? DHCPREQUEST : DHCPDISCOVER);
    if (requesting) {
        /* Which address, and whose offer of it: with two servers answering,
         * both see the request and the one not named withdraws its offer. */
        at = put_option(at, OPTION_REQUESTED_IP, client->lease.ip, IPV4_BYTES);
        at = put_option(at, OPTION_SERVER_ID, client->lease.server, IPV4_BYTES);
    }
    *at = OPTION_END;
    return DHCP_PAYLOAD_BYTES;
}

/* What `payload` is, and what it said.  Anything that is not a reply to this
 * machine's own exchange -- another client's, or another run of ours -- reads
 * as DhcpNothing. */
static enum DhcpMessage dhcp_parse(const uint8_t* payload,
    uint16_t length,
    const struct DhcpClient* client,
    struct DhcpLease* out) {
    if (length < DHCP_OPTIONS_AT) {
        return DhcpNothing;
    }
    if (payload[BOOTP_OP] != BOOTREPLY || payload[BOOTP_HLEN] != MAC_BYTES) {
        return DhcpNothing;
    }
    if (!net_same(&payload[BOOTP_COOKIE], MAGIC_COOKIE, sizeof MAGIC_COOKIE)) {
        return DhcpNothing;
    }
    /* Ours, and this run's: a LAN carries other clients' exchanges, and every
     * reply here is a broadcast that arrives whoever it was for. */
    if (!net_same(&payload[BOOTP_XID], client->xid, sizeof client->xid)) {
        return DhcpNothing;
    }
    if (!net_same(&payload[BOOTP_CHADDR], client->mac, MAC_BYTES)) {
        return DhcpNothing;
    }

    /* Written straight into the caller's scratch, which is not the lease:
     * whether anything read here is worth keeping is dhcp_step()'s to decide,
     * and it is the only one that can -- a second server's offer is perfectly
     * well formed and still must not land. */
    struct DhcpLease* found = out;
    *found = (struct DhcpLease){0};
    net_copy(found->ip, &payload[BOOTP_YIADDR], IPV4_BYTES);

    enum DhcpMessage message = DhcpNothing;
    uint16_t at = DHCP_OPTIONS_AT;
    while (at < length) {
        const uint8_t option = payload[at];
        if (option == OPTION_END) {
            break;
        }
        if (option == OPTION_PAD) {
            at++;
            continue;
        }
        /* Every length below comes off the wire, so each is checked against
         * what is left before it is trusted to index. */
        if ((uint16_t)(at + 2) > length) {
            break;
        }
        const uint8_t option_length = payload[at + 1];
        const uint16_t value = (uint16_t)(at + 2);
        /* Written as a subtraction, and in sixteen bits: the test above puts
         * `value` no further than `length`, so this cannot wrap, and widening
         * it to 32 bits cost a four-byte compare on every option. */
        if (option_length > (uint16_t)(length - value)) {
            break;
        }

        if (option == OPTION_MESSAGE_TYPE) {
            if (option_length == 1) {
                message = (enum DhcpMessage)payload[value];
            }
        } else if (option_length >= IPV4_BYTES) {
            /* An option carrying more than one address -- a list of routers,
             * say -- gives up its first, which is the one to use. */
            for (uint8_t i = 0; i < ADDRESS_OPTIONS; i++) {
                if (ADDRESS_OPTION[i] == option) {
                    net_copy((uint8_t*)found + ADDRESS_FIELD[i], &payload[value], IPV4_BYTES);
                    break;
                }
            }
        }
        at = value + option_length;
    }

    if (message != DhcpOffer && message != DhcpAck && message != DhcpNak) {
        return DhcpNothing;
    }
    return message;
}

void dhcp_start(struct DhcpClient* client, const uint8_t* mac, uint16_t seed) {
    *client = (struct DhcpClient){.stage = DhcpDiscovering};
    net_copy(client->mac, mac, MAC_BYTES);
    /* Wire order, big-endian like everything else; the two halves do the two
     * jobs dhcp.h describes. */
    client->xid[0] = (uint8_t)(seed >> 8);
    client->xid[1] = (uint8_t)seed;
    client->xid[2] = mac[4];
    client->xid[3] = mac[5];
}

/* Who the exchange is between.  A client with no address yet has to shout: the
 * server is reached by broadcast at both layers, and RFC 2131 has the reply
 * come back the same way (the flag build() sets), so an all-zero address of our
 * own is right rather than merely unavoidable -- udp_parse() reads it as
 * "accept any destination". */
static void endpoints(
    const struct DhcpClient* client, struct NetEndpoint* us, struct NetEndpoint* server) {
    *us = (struct NetEndpoint){.port = DHCP_CLIENT_PORT};
    net_copy(us->mac, client->mac, MAC_BYTES);
    *server = (struct NetEndpoint){.port = DHCP_SERVER_PORT};
    memset(server->mac, 0xFF, MAC_BYTES);
    memset(server->ip, 0xFF, IPV4_BYTES);
}

uint16_t dhcp_step(struct DhcpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out) {
    struct NetEndpoint us;
    struct NetEndpoint server;
    endpoints(client, &us, &server);

    if (in_length) {
        struct Datagram datagram;
        if (!udp_parse(in, in_length, &us, &datagram)) {
            return 0;
        }
        if (datagram.from.port != DHCP_SERVER_PORT) {
            return 0;
        }
        /* Beside the lease rather than over it, so a reply that turns out not
         * to be worth having leaves the one already accepted alone. */
        struct DhcpLease found;
        switch (dhcp_parse(&in[UDP_PAYLOAD_AT], datagram.payload_length, client, &found)) {
            case DhcpOffer:
                /* One offer is taken and the rest ignored: a second server's is as
                 * well formed as the first, and accepting it here would ask one
                 * server for the other's address. */
                if (client->stage != DhcpDiscovering) {
                    return 0;
                }
                client->lease = found;
                client->stage = DhcpRequesting;
                break;
            case DhcpAck:
                /* Only ever an answer to a request.  RFC 2131 has none that
                 * answers a discover, so one arriving here is a stray from an
                 * earlier exchange and would install a lease never asked for. */
                if (client->stage != DhcpRequesting) {
                    return 0;
                }
                client->lease = found;
                client->stage = DhcpLeased;
                break;
            case DhcpNak:
                /* The address asked for is not ours to have.  Back to the start,
                 * which the build below does at once rather than after a timeout. */
                client->stage = DhcpDiscovering;
                break;
            default:
                return 0;
        }
    }

    if (client->stage == DhcpLeased) {
        return 0;
    }
    /* Built where it will be sent from, so udp_build() copies the payload onto
     * itself -- 300 bytes of no-op, against a second 300-byte buffer in the
     * 16-bit window if it were built anywhere else. */
    const uint16_t length = say(client, &out[UDP_PAYLOAD_AT]);
    return udp_build(out, &us, &server, &out[UDP_PAYLOAD_AT], length);
}
