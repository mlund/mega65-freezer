// SPDX-License-Identifier: MIT OR Apache-2.0
#include "arp.h"

#include <string.h>

/* Offsets into the ARP payload, which follows the ethernet header eth.h
 * describes. */
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

uint16_t arp_request(
    uint8_t* frame, const uint8_t* mac, const uint8_t* sender_ip, const uint8_t* target_ip) {
    for (uint8_t i = 0; i < ARP_FRAME_BYTES; i++) {
        frame[i] = 0;
    }

    /* Broadcast: the point of asking is that nobody knows the answer yet. */
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        frame[ETH_DESTINATION + i] = 0xFF;
    }
    memcpy(&frame[ETH_SOURCE], mac, MAC_BYTES);
    net_put16(&frame[ETH_TYPE], ETHERTYPE_ARP);

    net_put16(&frame[ARP_HTYPE], ARP_ETHERNET);
    net_put16(&frame[ARP_PTYPE], ARP_IPV4);
    frame[ARP_HLEN] = MAC_BYTES;
    frame[ARP_PLEN] = IPV4_BYTES;
    net_put16(&frame[ARP_OPER], ARP_REQUEST);
    memcpy(&frame[ARP_SENDER_MAC], mac, MAC_BYTES);
    memcpy(&frame[ARP_SENDER_IP], sender_ip, IPV4_BYTES);
    /* Target hardware address stays zero: it is what is being asked for. */
    memcpy(&frame[ARP_TARGET_IP], target_ip, IPV4_BYTES);

    return ARP_FRAME_BYTES;
}

bool arp_reply_from(const uint8_t* frame, uint16_t length, const uint8_t* ip, uint8_t* mac_out) {
    if (length < ARP_FRAME_BYTES) {
        return false;
    }
    if (net_get16(&frame[ETH_TYPE]) != ETHERTYPE_ARP) {
        return false;
    }
    if (net_get16(&frame[ARP_PTYPE]) != ARP_IPV4 || frame[ARP_HLEN] != MAC_BYTES ||
        frame[ARP_PLEN] != IPV4_BYTES) {
        return false;
    }
    if (net_get16(&frame[ARP_OPER]) != ARP_REPLY) {
        return false;
    }
    if (!net_same(&frame[ARP_SENDER_IP], ip, IPV4_BYTES)) {
        return false;
    }
    memcpy(mac_out, &frame[ARP_SENDER_MAC], MAC_BYTES);
    return true;
}
