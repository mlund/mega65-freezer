// SPDX-License-Identifier: MIT OR Apache-2.0
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
static constexpr uint8_t OPTION_TFTP_ADDRESS = 150;
static constexpr uint8_t OPTION_PAD = 0;
static constexpr uint8_t OPTION_END = 255;

static constexpr uint8_t DHCPDISCOVER = 1;
static constexpr uint8_t DHCPREQUEST = 3;

/* What is asked for.  Option 66 is absent deliberately: it names a TFTP server
 * rather than addressing one, and resolving a name needs a resolver this does
 * not have yet.  Option 6 is asked for even so -- it is the address of the
 * resolver, so it is the thing that would make 66 usable rather than another
 * thing that needs one. */
static const uint8_t WANTED[] = {OPTION_NETMASK, OPTION_ROUTER, OPTION_DNS, OPTION_TFTP_ADDRESS};

static constexpr uint8_t ADDRESS_OPTIONS = 5;
/* The options that carry one address and differ only in where it belongs.
 * A table rather than five near-identical cases: each case re-derives the
 * source pointer, and five of them came to two fifths of the parser. */
static const uint8_t ADDRESS_OPTION[ADDRESS_OPTIONS] = {
    OPTION_NETMASK, OPTION_ROUTER, OPTION_DNS, OPTION_SERVER_ID, OPTION_TFTP_ADDRESS};
static const uint8_t ADDRESS_FIELD[ADDRESS_OPTIONS] = {
    offsetof(struct DhcpLease, netmask),
    offsetof(struct DhcpLease, router),
    offsetof(struct DhcpLease, dns),
    offsetof(struct DhcpLease, server),
    offsetof(struct DhcpLease, tftp),
};

/* A field of the lease, by its offset in the table above. */
static uint8_t* out_field(struct DhcpLease* lease, uint8_t at) {
    return (uint8_t*)lease + at;
}

static uint8_t* put_option(uint8_t* at, uint8_t option, const uint8_t* value, uint8_t length) {
    *at++ = option;
    *at++ = length;
    memcpy(at, value, length);
    return at + length;
}

/* Everything the two messages share: a request from this machine, for this
 * exchange, saying who it is and what it would like to be told. */
static uint8_t* build(uint8_t* payload, const uint8_t* mac, uint32_t xid, uint8_t type) {
    memset(payload, 0, DHCP_PAYLOAD_BYTES);

    payload[BOOTP_OP] = BOOTREQUEST;
    payload[BOOTP_HTYPE] = HTYPE_ETHERNET;
    payload[BOOTP_HLEN] = MAC_BYTES;
    /* The exchange identifier is echoed back, and is how a reply is known to
     * be ours.  Big-endian like everything else on the wire. */
    payload[BOOTP_XID + 0] = (uint8_t)(xid >> 24);
    payload[BOOTP_XID + 1] = (uint8_t)(xid >> 16);
    payload[BOOTP_XID + 2] = (uint8_t)(xid >> 8);
    payload[BOOTP_XID + 3] = (uint8_t)xid;
    net_put16(&payload[BOOTP_FLAGS], BOOTP_BROADCAST);
    memcpy(&payload[BOOTP_CHADDR], mac, MAC_BYTES);
    memcpy(&payload[BOOTP_COOKIE], MAGIC_COOKIE, sizeof MAGIC_COOKIE);

    uint8_t* at = &payload[DHCP_OPTIONS_AT];
    at = put_option(at, OPTION_MESSAGE_TYPE, &type, 1);
    return put_option(at, OPTION_PARAMETER_LIST, WANTED, sizeof WANTED);
}

/* The length is always the full one: build() zeroed the buffer, so everything
 * past the end option is already the pad option, and RFC 2131 asks a client to
 * send at least this much. */
uint16_t dhcp_discover(uint8_t* payload, const uint8_t* mac, uint32_t xid) {
    uint8_t* at = build(payload, mac, xid, DHCPDISCOVER);
    *at = OPTION_END;
    return DHCP_PAYLOAD_BYTES;
}

uint16_t dhcp_request(
    uint8_t* payload, const uint8_t* mac, uint32_t xid, const struct DhcpLease* offer) {
    uint8_t* at = build(payload, mac, xid, DHCPREQUEST);
    /* Which address, and whose offer of it: with two servers answering, both
     * see the request and the one not named withdraws its offer. */
    at = put_option(at, OPTION_REQUESTED_IP, offer->ip, IPV4_BYTES);
    at = put_option(at, OPTION_SERVER_ID, offer->server, IPV4_BYTES);
    *at = OPTION_END;
    return DHCP_PAYLOAD_BYTES;
}

enum DhcpMessage dhcp_parse(const uint8_t* payload,
    uint16_t length,
    uint32_t xid,
    const uint8_t* mac,
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
    if (payload[BOOTP_XID + 0] != (uint8_t)(xid >> 24) ||
        payload[BOOTP_XID + 1] != (uint8_t)(xid >> 16) ||
        payload[BOOTP_XID + 2] != (uint8_t)(xid >> 8) || payload[BOOTP_XID + 3] != (uint8_t)xid) {
        return DhcpNothing;
    }
    if (!net_same(&payload[BOOTP_CHADDR], mac, MAC_BYTES)) {
        return DhcpNothing;
    }

    /* Filled here and copied out only once the message turns out to be one
     * worth having: a NAK, a second server's offer, or a reply carrying our
     * exchange id but nothing usable must not wipe a lease already taken. */
    struct DhcpLease found = {0};
    memcpy(found.ip, &payload[BOOTP_YIADDR], IPV4_BYTES);

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
        if ((uint32_t)value + option_length > length) {
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
                    memcpy(
                        (uint8_t*)out_field(&found, ADDRESS_FIELD[i]), &payload[value], IPV4_BYTES);
                    if (option == OPTION_TFTP_ADDRESS) {
                        found.has_tftp = true;
                    }
                    break;
                }
            }
        }
        at = value + option_length;
    }

    if (message != DhcpOffer && message != DhcpAck && message != DhcpNak) {
        return DhcpNothing;
    }
    /* A refusal carries no lease, so it leaves the caller's alone as well. */
    if (message != DhcpNak) {
        *out = found;
    }
    return message;
}
