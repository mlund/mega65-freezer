// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* Where the fields of an ARP frame are, and the three things built from that
 * one layout: a question, an answer, and the reading of a reply. */

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
static constexpr uint8_t ARP_TARGET_MAC = ETH_HEADER_BYTES + 18;
static constexpr uint8_t ARP_TARGET_IP = ETH_HEADER_BYTES + 24;

static constexpr uint16_t ARP_ETHERNET = 1;
static constexpr uint16_t ARP_IPV4 = 0x0800;
static constexpr uint16_t ARP_REQUEST = 1;
static constexpr uint16_t ARP_REPLY = 2;

/* One builder for both messages: RFC 826 makes a reply the question turned
 * round, and writing them as two frames made that a coincidence of twenty
 * identical lines rather than a fact.
 *
 * The addresses are read into locals before anything is written, which is what
 * lets a caller answer a request in the buffer it arrived in -- and puts that
 * guarantee in one function instead of a warning in the header. */
static uint16_t arp_frame(uint8_t* frame,
    uint16_t operation,
    const uint8_t* mac,
    const uint8_t* sender_ip,
    const uint8_t* peer_mac,
    const uint8_t* peer_ip) {
    uint8_t their_mac[MAC_BYTES];
    uint8_t their_ip[IPV4_BYTES];
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        their_mac[i] = peer_mac ? peer_mac[i] : 0xFF;
    }
    net_copy(their_ip, peer_ip, IPV4_BYTES);

    for (uint8_t i = 0; i < ARP_FRAME_BYTES; i++) {
        frame[i] = 0;
    }
    /* A question goes to everyone because nobody knows the answer yet; an
     * answer goes only to whoever asked. */
    net_copy(&frame[ETH_DESTINATION], their_mac, MAC_BYTES);
    net_copy(&frame[ETH_SOURCE], mac, MAC_BYTES);
    net_put16(&frame[ETH_TYPE], ETHERTYPE_ARP);

    net_put16(&frame[ARP_HTYPE], ARP_ETHERNET);
    net_put16(&frame[ARP_PTYPE], ARP_IPV4);
    frame[ARP_HLEN] = MAC_BYTES;
    frame[ARP_PLEN] = IPV4_BYTES;
    net_put16(&frame[ARP_OPER], operation);
    net_copy(&frame[ARP_SENDER_MAC], mac, MAC_BYTES);
    net_copy(&frame[ARP_SENDER_IP], sender_ip, IPV4_BYTES);
    /* A request leaves the target hardware address zero: it is the question. */
    if (operation == ARP_REPLY) {
        net_copy(&frame[ARP_TARGET_MAC], their_mac, MAC_BYTES);
    }
    net_copy(&frame[ARP_TARGET_IP], their_ip, IPV4_BYTES);
    return ARP_FRAME_BYTES;
}

/* The parts every ARP frame must have before any of its fields mean
 * anything. */
static bool is_arp(const uint8_t* frame, uint16_t length, uint16_t operation) {
    return length >= ARP_FRAME_BYTES && net_get16(&frame[ETH_TYPE]) == ETHERTYPE_ARP &&
        net_get16(&frame[ARP_PTYPE]) == ARP_IPV4 && frame[ARP_HLEN] == MAC_BYTES &&
        frame[ARP_PLEN] == IPV4_BYTES && net_get16(&frame[ARP_OPER]) == operation;
}

uint16_t arp_request(
    uint8_t* frame, const uint8_t* mac, const uint8_t* sender_ip, const uint8_t* target_ip) {
    return arp_frame(frame, ARP_REQUEST, mac, sender_ip, nullptr, target_ip);
}

bool arp_reply_from(const uint8_t* frame, uint16_t length, const uint8_t* ip, uint8_t* mac_out) {
    if (!is_arp(frame, length, ARP_REPLY)) {
        return false;
    }
    if (!net_same(&frame[ARP_SENDER_IP], ip, IPV4_BYTES)) {
        return false;
    }
    net_copy(mac_out, &frame[ARP_SENDER_MAC], MAC_BYTES);
    return true;
}

uint16_t arp_answer(uint8_t* frame, uint16_t length, const uint8_t* mac, const uint8_t* ip) {
    /* Nothing to defend before a lease: an address of all zeros is this
     * machine saying it has none yet, not one it could answer for. */
    if (net_zero(ip, IPV4_BYTES) || !is_arp(frame, length, ARP_REQUEST)) {
        return 0;
    }
    if (!net_same(&frame[ARP_TARGET_IP], ip, IPV4_BYTES)) {
        return 0;
    }
    return arp_frame(frame, ARP_REPLY, mac, ip, &frame[ARP_SENDER_MAC], &frame[ARP_SENDER_IP]);
}
