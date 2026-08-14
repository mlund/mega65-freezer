// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ip.h"

#include <string.h>

/* Offsets into the IPv4 header, which follows the ethernet one. */
static constexpr uint8_t IP_VERSION_IHL = ETH_HEADER_BYTES + 0;
static constexpr uint8_t IP_TOTAL_LENGTH = ETH_HEADER_BYTES + 2;
static constexpr uint8_t IP_IDENTIFICATION = ETH_HEADER_BYTES + 4;
static constexpr uint8_t IP_FRAGMENT = ETH_HEADER_BYTES + 6;
static constexpr uint8_t IP_TTL = ETH_HEADER_BYTES + 8;
static constexpr uint8_t IP_PROTOCOL = ETH_HEADER_BYTES + 9;
static constexpr uint8_t IP_CHECKSUM = ETH_HEADER_BYTES + 10;
static constexpr uint8_t IP_SOURCE = ETH_HEADER_BYTES + 12;
static constexpr uint8_t IP_DESTINATION = ETH_HEADER_BYTES + 16;

/* And into the UDP header, which follows a 20-byte IPv4 one. */
static constexpr uint8_t UDP_AT = ETH_HEADER_BYTES + IPV4_HEADER_BYTES;
static constexpr uint8_t UDP_SOURCE_PORT = UDP_AT + 0;
static constexpr uint8_t UDP_DESTINATION_PORT = UDP_AT + 2;
static constexpr uint8_t UDP_LENGTH = UDP_AT + 4;
static constexpr uint8_t UDP_CHECKSUM = UDP_AT + 6;

static constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
/* Version 4, and a header of five 32-bit words: no options, which is what
 * lets every offset above be a constant. */
static constexpr uint8_t IPV4_VERSION_IHL = 0x45;
static constexpr uint8_t IP_PROTOCOL_UDP = 17;
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

static bool is_zero(const uint8_t* a, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (a[i]) {
            return false;
        }
    }
    return true;
}

uint32_t ip_sum(uint32_t sum, const uint8_t* data, uint16_t length) {
    uint16_t i = 0;
    for (; i + 1 < length; i += 2) {
        sum += net_get16(&data[i]);
    }
    /* An odd tail pads with a zero low byte, which is why only the last piece
     * of a multi-part sum may be odd. */
    if (i < length) {
        sum += (uint32_t)data[i] << 8;
    }
    return sum;
}

uint16_t ip_sum_final(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* The twelve bytes RFC 768 has a UDP checksum cover but does not transmit:
 * the addresses, the protocol and the UDP length again. */
static uint32_t pseudo_header_sum(
    const uint8_t* source, const uint8_t* destination, uint16_t udp_length) {
    uint32_t sum = ip_sum(0, source, IPV4_BYTES);
    sum = ip_sum(sum, destination, IPV4_BYTES);
    sum += IP_PROTOCOL_UDP;
    sum += udp_length;
    return sum;
}

uint16_t udp_build(uint8_t* frame,
    const struct NetEndpoint* from,
    const struct NetEndpoint* to,
    const uint8_t* payload,
    uint16_t payload_length) {
    const uint16_t udp_length = UDP_HEADER_BYTES + payload_length;
    const uint16_t total_length = IPV4_HEADER_BYTES + udp_length;

    memcpy(&frame[ETH_DESTINATION], to->mac, MAC_BYTES);
    memcpy(&frame[ETH_SOURCE], from->mac, MAC_BYTES);
    net_put16(&frame[ETH_TYPE], ETHERTYPE_IPV4);

    frame[IP_VERSION_IHL] = IPV4_VERSION_IHL;
    frame[IP_VERSION_IHL + 1] = 0; /* no differentiated services */
    net_put16(&frame[IP_TOTAL_LENGTH], total_length);
    net_put16(&frame[IP_IDENTIFICATION], 0);
    net_put16(&frame[IP_FRAGMENT], IP_DONT_FRAGMENT);
    frame[IP_TTL] = IP_DEFAULT_TTL;
    frame[IP_PROTOCOL] = IP_PROTOCOL_UDP;
    net_put16(&frame[IP_CHECKSUM], 0); /* zero while it is being computed over */
    memcpy(&frame[IP_SOURCE], from->ip, IPV4_BYTES);
    memcpy(&frame[IP_DESTINATION], to->ip, IPV4_BYTES);
    net_put16(
        &frame[IP_CHECKSUM], ip_sum_final(ip_sum(0, &frame[IP_VERSION_IHL], IPV4_HEADER_BYTES)));

    net_put16(&frame[UDP_SOURCE_PORT], from->port);
    net_put16(&frame[UDP_DESTINATION_PORT], to->port);
    net_put16(&frame[UDP_LENGTH], udp_length);
    net_put16(&frame[UDP_CHECKSUM], 0);
    for (uint16_t i = 0; i < payload_length; i++) {
        frame[UDP_PAYLOAD_AT + i] = payload[i];
    }

    uint32_t sum = pseudo_header_sum(from->ip, to->ip, udp_length);
    sum = ip_sum(sum, &frame[UDP_AT], udp_length);
    const uint16_t checksum = ip_sum_final(sum);
    /* A computed zero is sent as all ones: zero on the wire is reserved for
     * "not computed", and the two are the same value in one's complement. */
    net_put16(&frame[UDP_CHECKSUM], checksum ? checksum : 0xFFFF);

    return UDP_PAYLOAD_AT + payload_length;
}

bool udp_parse(
    const uint8_t* frame, uint16_t length, const struct NetEndpoint* us, struct UdpDatagram* out) {
    if (length < UDP_PAYLOAD_AT) {
        return false;
    }
    if (net_get16(&frame[ETH_TYPE]) != ETHERTYPE_IPV4) {
        return false;
    }
    /* Options would move every offset below, and nothing on a LAN sends them. */
    if (frame[IP_VERSION_IHL] != IPV4_VERSION_IHL) {
        return false;
    }
    if (frame[IP_PROTOCOL] != IP_PROTOCOL_UDP) {
        return false;
    }
    if (net_get16(&frame[IP_FRAGMENT]) & IP_FRAGMENTED) {
        return false;
    }
    if (ip_sum_final(ip_sum(0, &frame[IP_VERSION_IHL], IPV4_HEADER_BYTES))) {
        return false;
    }
    /* An all-zero address accepts any destination, for a machine that has not
     * been given one yet. */
    if (!is_zero(us->ip, IPV4_BYTES) && !net_same(&frame[IP_DESTINATION], us->ip, IPV4_BYTES)) {
        return false;
    }
    if (net_get16(&frame[UDP_DESTINATION_PORT]) != us->port) {
        return false;
    }

    /* What the IP header says the datagram is, against what the UDP header
     * claims.  Without this, a frame padded out to ethernet's 60-byte minimum
     * can have its UDP length inflated to cover the padding, and the padding
     * comes back as payload. */
    const uint16_t udp_length = net_get16(&frame[UDP_LENGTH]);
    if (net_get16(&frame[IP_TOTAL_LENGTH]) != (uint16_t)(IPV4_HEADER_BYTES + udp_length)) {
        return false;
    }
    /* Written as a subtraction because the addition wraps: UDP_AT + 0xFFFF is
     * 33 in sixteen bits, which passes any length, and the sum below would
     * then read 64KB past the caller's buffer.  The early length test above
     * guarantees length >= UDP_PAYLOAD_AT, so this cannot underflow. */
    if (udp_length < UDP_HEADER_BYTES || udp_length > (uint16_t)(length - UDP_AT)) {
        return false;
    }

    const uint16_t checksum = net_get16(&frame[UDP_CHECKSUM]);
    if (checksum) {
        uint32_t sum = pseudo_header_sum(&frame[IP_SOURCE], &frame[IP_DESTINATION], udp_length);
        sum = ip_sum(sum, &frame[UDP_AT], udp_length);
        if (ip_sum_final(sum)) {
            return false;
        }
    }

    memcpy(out->from.ip, &frame[IP_SOURCE], IPV4_BYTES);
    memcpy(out->from.mac, &frame[ETH_SOURCE], MAC_BYTES);
    out->from.port = net_get16(&frame[UDP_SOURCE_PORT]);
    out->payload_length = udp_length - UDP_HEADER_BYTES;
    return true;
}
