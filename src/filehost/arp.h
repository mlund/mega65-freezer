// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ARP, RFC 826: the one exchange that needs no IP address of our own and no
 * checksum, which is what makes it the first thing to put on the wire.
 *
 * Building and reading the frames is separate from sending them (eth.h), so
 * this compiles for the host and is tested there -- the emulator has no
 * ethernet on macOS, so a test on the wire is a test on hardware. */

constexpr uint8_t MAC_BYTES = 6;
constexpr uint8_t IPV4_BYTES = 4;
/* 14 bytes of ethernet header and 28 of ARP. */
constexpr uint8_t ARP_FRAME_BYTES = 42;
constexpr uint16_t ETHERTYPE_ARP = 0x0806;

/* Fills `frame` with a request asking who has `target_ip`.  Returns its
 * length.  `sender_ip` may be all zeros -- an ARP probe in the sense of RFC
 * 5227, which is what a machine that has not yet asked for a lease can send. */
uint16_t arp_request(
    uint8_t* frame, const uint8_t* mac, const uint8_t* sender_ip, const uint8_t* target_ip);

/* True when `frame` is an ARP reply from `ip`, and then `mac_out` is its
 * hardware address. */
bool arp_reply_from(const uint8_t* frame, uint16_t length, const uint8_t* ip, uint8_t* mac_out);
