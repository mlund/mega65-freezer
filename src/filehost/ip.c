// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* Where the fields of an IPv4 and a UDP header are, the one's-complement sum
 * that covers them, and the datagram built and checked from both. */

#include "ip.h"

#include <string.h>

/* Offsets into the IPv4 header, which follows the ethernet one. */
static constexpr uint8_t IP_AT = ETH_HEADER_BYTES;
static constexpr uint8_t IP_VERSION_IHL = IP_AT + 0;
static constexpr uint8_t IP_TOTAL_LENGTH = IP_AT + 2;
static constexpr uint8_t IP_IDENTIFICATION = IP_AT + 4;
static constexpr uint8_t IP_FRAGMENT = IP_AT + 6;
static constexpr uint8_t IP_TTL = IP_AT + 8;
static constexpr uint8_t IP_PROTOCOL = IP_AT + 9;
static constexpr uint8_t IP_CHECKSUM = IP_AT + 10;
static constexpr uint8_t IP_SOURCE = IP_AT + 12;
static constexpr uint8_t IP_DESTINATION = IP_AT + 16;

/* And into the UDP header, which follows a 20-byte IPv4 one. */
/* IPV4_PAYLOAD_AT by another name, and the same number: UDP begins where IPv4
 * stops. */
static constexpr uint8_t UDP_AT = IPV4_PAYLOAD_AT;
static constexpr uint8_t UDP_SOURCE_PORT = UDP_AT + 0;
static constexpr uint8_t UDP_DESTINATION_PORT = UDP_AT + 2;
static constexpr uint8_t UDP_LENGTH = UDP_AT + 4;
static constexpr uint8_t UDP_CHECKSUM = UDP_AT + 6;

static constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
/* Version 4, and a header of five 32-bit words: no options, which is what
 * lets every offset above be a constant. */
static constexpr uint8_t IPV4_VERSION_IHL = 0x45;
/* Plenty for a LAN, and the value nothing has to think about. */
static constexpr uint8_t IP_DEFAULT_TTL = 64;
/* The bits of the fragment field that say a datagram is in pieces: the
 * more-fragments flag, and any non-zero offset.  The don't-fragment bit above
 * them is deliberately not among them. */
static constexpr uint16_t IP_FRAGMENTED = 0x3FFF;
/* Don't fragment.  With it set and no offset the datagram is atomic, and
 * RFC 6864 lets an atomic datagram carry any identification at all -- so there
 * is no counter to keep, and udp_build() stays a function of its arguments. */
static constexpr uint16_t IP_DONT_FRAGMENT = 0x4000;

uint32_t ip_sum(uint32_t sum, const uint8_t* data, uint16_t length) {
    /* Sixteen bits with the carry folded back in as it happens, rather than a
     * 32-bit accumulator carried to the end.  RFC 1071 section 1.2C: adding
     * the carry back at the bottom is the same answer, and it costs two 8-bit
     * adds and a branch where the wide accumulator cost two more `adc #0`
     * chains and their stores on every word.  Measured at 40% fewer cycles on
     * the one loop both fetches pay for every byte received.
     *
     * The upper half of `sum` is passed through untouched: a caller chains
     * pieces through here and ip_sum_final() folds whatever is left. */
    uint16_t total = (uint16_t)sum;
    for (; length >= 2; length -= 2, data += 2) {
        const uint16_t word = net_get16(data);
        total = (uint16_t)(total + word);
        if (total < word) {
            total++;
        }
    }
    /* An odd tail pads with a zero low byte, which is why only the last piece
     * of a multi-part sum may be odd. */
    if (length) {
        const uint16_t word = (uint16_t)((uint16_t)*data << 8);
        total = (uint16_t)(total + word);
        if (total < word) {
            total++;
        }
    }
    return (sum & 0xFFFF0000UL) | total;
}

bool ip_parse(const char* text, uint8_t* out, uint16_t* port) {
    /* Into a copy, so a refusal partway leaves the caller's address alone --
     * half an address is worse than none, and the caller may be holding the
     * one it is already using. */
    uint8_t parsed[IPV4_BYTES];
    for (uint8_t i = 0; i < IPV4_BYTES; i++) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        uint16_t value = 0;
        while (*text >= '0' && *text <= '9') {
            value = (uint16_t)(value * 10 + (uint8_t)(*text++ - '0'));
            if (value > 255) {
                return false;
            }
        }
        parsed[i] = (uint8_t)value;
        if (i < IPV4_BYTES - 1 && *text++ != '.') {
            return false;
        }
    }
    uint16_t named = 0;
    if (*text == ':') {
        text++;
        if (*text < '0' || *text > '9') {
            return false;
        }
        /* Sixteen bits, checked before the multiply rather than after.  An
         * accumulator wide enough to overflow into is the obvious way to write
         * it and measured 84 bytes dearer: the multiply and the compare are
         * both inlined, in 32 bits, for a number that never exceeds 65535. */
        uint16_t value = 0;
        while (*text >= '0' && *text <= '9') {
            const uint8_t digit = (uint8_t)(*text++ - '0');
            if (value > 6553) {
                return false;
            }
            value = (uint16_t)(value * 10 + digit);
            if (value < digit) {
                return false; /* the one the check above lets through */
            }
        }
        /* Port zero is reserved and nothing listens on it, so a text that says
         * so is a mistake rather than an instruction. */
        if (!value) {
            return false;
        }
        named = (uint16_t)value;
    }
    if (*text && *text != ' ' && *text != '\r' && *text != '\n') {
        return false;
    }

    net_copy(out, parsed, IPV4_BYTES);
    if (named) {
        *port = named;
    }
    return true;
}

const uint8_t* ip_next_hop(
    const uint8_t* target, const uint8_t* us, const uint8_t* netmask, const uint8_t* router) {
    for (uint8_t i = 0; i < IPV4_BYTES; i++) {
        /* The bits the mask keeps are the network number; a difference in any
         * of them puts the target on somebody else's. */
        if ((target[i] ^ us[i]) & netmask[i]) {
            return router;
        }
    }
    return target;
}

uint16_t ip_sum_final(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint32_t ipv4_pseudo_sum(
    const uint8_t* source, const uint8_t* destination, uint8_t protocol, uint16_t payload_length) {
    uint32_t sum = ip_sum(0, source, IPV4_BYTES);
    sum = ip_sum(sum, destination, IPV4_BYTES);
    sum += protocol;
    sum += payload_length;
    return sum;
}

void ipv4_build_header(uint8_t* frame,
    const struct NetEndpoint* from,
    const struct NetEndpoint* to,
    uint8_t protocol,
    uint16_t payload_length) {
    net_copy(&frame[ETH_DESTINATION], to->mac, MAC_BYTES);
    net_copy(&frame[ETH_SOURCE], from->mac, MAC_BYTES);
    net_put16(&frame[ETH_TYPE], ETHERTYPE_IPV4);

    frame[IP_VERSION_IHL] = IPV4_VERSION_IHL;
    frame[IP_VERSION_IHL + 1] = 0; /* no differentiated services */
    net_put16(&frame[IP_TOTAL_LENGTH], (uint16_t)(IPV4_HEADER_BYTES + payload_length));
    net_put16(&frame[IP_IDENTIFICATION], 0);
    net_put16(&frame[IP_FRAGMENT], IP_DONT_FRAGMENT);
    frame[IP_TTL] = IP_DEFAULT_TTL;
    frame[IP_PROTOCOL] = protocol;
    net_put16(&frame[IP_CHECKSUM], 0); /* zero while it is being computed over */
    net_copy(&frame[IP_SOURCE], from->ip, IPV4_BYTES);
    net_copy(&frame[IP_DESTINATION], to->ip, IPV4_BYTES);
    net_put16(&frame[IP_CHECKSUM], ip_sum_final(ip_sum(0, &frame[IP_AT], IPV4_HEADER_BYTES)));
}

bool ipv4_parse(const uint8_t* frame,
    uint16_t length,
    const struct NetEndpoint* us,
    uint8_t protocol,
    struct Datagram* out) {
    if (length < IPV4_PAYLOAD_AT) {
        return false;
    }
    if (net_get16(&frame[ETH_TYPE]) != ETHERTYPE_IPV4) {
        return false;
    }
    /* Options would move every offset above, and nothing on a LAN sends them. */
    if (frame[IP_VERSION_IHL] != IPV4_VERSION_IHL) {
        return false;
    }
    if (frame[IP_PROTOCOL] != protocol) {
        return false;
    }
    if (net_get16(&frame[IP_FRAGMENT]) & IP_FRAGMENTED) {
        return false;
    }
    if (ip_sum_final(ip_sum(0, &frame[IP_AT], IPV4_HEADER_BYTES))) {
        return false;
    }
    /* An all-zero address accepts any destination, for a machine that has not
     * been given one yet. */
    if (!net_zero(us->ip, IPV4_BYTES) && !net_same(&frame[IP_DESTINATION], us->ip, IPV4_BYTES)) {
        return false;
    }

    /* What the header says is there, against what arrived.  Without the second
     * test a frame padded out to ethernet's 60-byte minimum can have its length
     * inflated to cover the padding -- or past the buffer entirely.  Written as
     * a subtraction because the addition wraps: IPV4_PAYLOAD_AT + 0xFFFF is 33
     * in sixteen bits, which passes any length at all. */
    const uint16_t total_length = net_get16(&frame[IP_TOTAL_LENGTH]);
    if (total_length < IPV4_HEADER_BYTES) {
        return false;
    }
    const uint16_t payload_length = (uint16_t)(total_length - IPV4_HEADER_BYTES);
    if (payload_length > (uint16_t)(length - IPV4_PAYLOAD_AT)) {
        return false;
    }

    net_copy(out->from.ip, &frame[IP_SOURCE], IPV4_BYTES);
    net_copy(out->from.mac, &frame[ETH_SOURCE], MAC_BYTES);
    out->payload_length = payload_length;
    return true;
}

uint16_t udp_build(uint8_t* frame,
    const struct NetEndpoint* from,
    const struct NetEndpoint* to,
    const uint8_t* payload,
    uint16_t payload_length) {
    const uint16_t udp_length = UDP_HEADER_BYTES + payload_length;
    ipv4_build_header(frame, from, to, IPV4_PROTOCOL_UDP, udp_length);

    net_put16(&frame[UDP_SOURCE_PORT], from->port);
    net_put16(&frame[UDP_DESTINATION_PORT], to->port);
    net_put16(&frame[UDP_LENGTH], udp_length);
    net_put16(&frame[UDP_CHECKSUM], 0);
    for (uint16_t i = 0; i < payload_length; i++) {
        frame[UDP_PAYLOAD_AT + i] = payload[i];
    }

    uint32_t sum = ipv4_pseudo_sum(from->ip, to->ip, IPV4_PROTOCOL_UDP, udp_length);
    sum = ip_sum(sum, &frame[UDP_AT], udp_length);
    const uint16_t checksum = ip_sum_final(sum);
    /* A computed zero is sent as all ones: zero on the wire is reserved for
     * "not computed", and the two are the same value in one's complement. */
    net_put16(&frame[UDP_CHECKSUM], checksum ? checksum : 0xFFFF);

    return UDP_PAYLOAD_AT + payload_length;
}

bool udp_parse(
    const uint8_t* frame, uint16_t length, const struct NetEndpoint* us, struct Datagram* out) {
    /* Into a local and copied out at the end, rather than parsed straight into
     * the caller's struct.  It reads worse and measured 11 bytes better: `out`
     * escapes, so every field read back off it is a reload the local keeps in
     * registers. */
    struct Datagram parsed;
    if (!ipv4_parse(frame, length, us, IPV4_PROTOCOL_UDP, &parsed)) {
        return false;
    }
    /* Below this the UDP header is read, which the layer underneath has now
     * guaranteed is there: a payload of eight bytes or more cannot be claimed
     * by a frame that does not hold them. */
    const uint16_t udp_length = parsed.payload_length;
    if (udp_length < UDP_HEADER_BYTES) {
        return false;
    }
    /* What the IP header says the datagram is, against what the UDP header
     * claims.  Without this, a frame padded out to ethernet's 60-byte minimum
     * can have its UDP length inflated to cover the padding, and the padding
     * comes back as payload. */
    if (net_get16(&frame[UDP_LENGTH]) != udp_length) {
        return false;
    }
    if (net_get16(&frame[UDP_DESTINATION_PORT]) != us->port) {
        return false;
    }

    const uint16_t checksum = net_get16(&frame[UDP_CHECKSUM]);
    if (checksum) {
        uint32_t sum = ipv4_pseudo_sum(
            &frame[IP_SOURCE], &frame[IP_DESTINATION], IPV4_PROTOCOL_UDP, udp_length);
        sum = ip_sum(sum, &frame[UDP_AT], udp_length);
        if (ip_sum_final(sum)) {
            return false;
        }
    }

    /* The endpoint alone: `payload_length` is UDP's own below, so copying the
     * whole struct here writes a field that is immediately overwritten. */
    out->from = parsed.from;
    out->from.port = net_get16(&frame[UDP_SOURCE_PORT]);
    out->payload_length = udp_length - UDP_HEADER_BYTES;
    return true;
}
