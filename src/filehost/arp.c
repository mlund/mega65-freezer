// SPDX-License-Identifier: MIT OR Apache-2.0
#include "arp.h"

/* Offsets into the ethernet header, then into the ARP payload that follows it.
 * Everything on the wire is big-endian, which is the opposite of both this CPU
 * and the catalogue, so the two-byte fields are written a byte at a time
 * rather than through a cast. */
static constexpr uint8_t ETH_DESTINATION = 0;
static constexpr uint8_t ETH_SOURCE = 6;
static constexpr uint8_t ETH_TYPE = 12;
static constexpr uint8_t ETH_HEADER_BYTES = 14;

static constexpr uint8_t ARP_HTYPE = ETH_HEADER_BYTES + 0;
static constexpr uint8_t ARP_PTYPE = ETH_HEADER_BYTES + 2;
static constexpr uint8_t ARP_HLEN = ETH_HEADER_BYTES + 4;
static constexpr uint8_t ARP_PLEN = ETH_HEADER_BYTES + 5;
static constexpr uint8_t ARP_OPER = ETH_HEADER_BYTES + 6;
static constexpr uint8_t ARP_SENDER_MAC = ETH_HEADER_BYTES + 8;
static constexpr uint8_t ARP_SENDER_IP = ETH_HEADER_BYTES + 14;
/* The target hardware address at +18 has no name here: it is the field being
 * asked about, so a request leaves it zero and a reply is read for its sender
 * address instead. */
static constexpr uint8_t ARP_TARGET_IP = ETH_HEADER_BYTES + 24;

static constexpr uint16_t ARP_ETHERNET = 1;
static constexpr uint16_t ARP_IPV4 = 0x0800;
static constexpr uint16_t ARP_REQUEST = 1;
static constexpr uint16_t ARP_REPLY = 2;

static void put16(uint8_t* at, uint16_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static uint16_t get16(const uint8_t* at) {
    return (uint16_t)(((uint16_t)at[0] << 8) | at[1]);
}

static void copy(uint8_t* out, const uint8_t* in, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        out[i] = in[i];
    }
}

static bool same(const uint8_t* a, const uint8_t* b, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

uint16_t arp_request(
    uint8_t* frame, const uint8_t* mac, const uint8_t* sender_ip, const uint8_t* target_ip) {
    for (uint8_t i = 0; i < ARP_FRAME_BYTES; i++) {
        frame[i] = 0;
    }

    /* Broadcast: the point of asking is that nobody knows the answer yet. */
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        frame[ETH_DESTINATION + i] = 0xFF;
    }
    copy(&frame[ETH_SOURCE], mac, MAC_BYTES);
    put16(&frame[ETH_TYPE], ETHERTYPE_ARP);

    put16(&frame[ARP_HTYPE], ARP_ETHERNET);
    put16(&frame[ARP_PTYPE], ARP_IPV4);
    frame[ARP_HLEN] = MAC_BYTES;
    frame[ARP_PLEN] = IPV4_BYTES;
    put16(&frame[ARP_OPER], ARP_REQUEST);
    copy(&frame[ARP_SENDER_MAC], mac, MAC_BYTES);
    copy(&frame[ARP_SENDER_IP], sender_ip, IPV4_BYTES);
    /* Target hardware address stays zero: it is what is being asked for. */
    copy(&frame[ARP_TARGET_IP], target_ip, IPV4_BYTES);

    return ARP_FRAME_BYTES;
}

bool arp_reply_from(const uint8_t* frame, uint16_t length, const uint8_t* ip, uint8_t* mac_out) {
    if (length < ARP_FRAME_BYTES) {
        return false;
    }
    if (get16(&frame[ETH_TYPE]) != ETHERTYPE_ARP) {
        return false;
    }
    if (get16(&frame[ARP_PTYPE]) != ARP_IPV4 || frame[ARP_HLEN] != MAC_BYTES ||
        frame[ARP_PLEN] != IPV4_BYTES) {
        return false;
    }
    if (get16(&frame[ARP_OPER]) != ARP_REPLY) {
        return false;
    }
    if (!same(&frame[ARP_SENDER_IP], ip, IPV4_BYTES)) {
        return false;
    }
    copy(mac_out, &frame[ARP_SENDER_MAC], MAC_BYTES);
    return true;
}
