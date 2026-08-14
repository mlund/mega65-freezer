// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The 45E100 ethernet controller.
 *
 * Registers and bits come from the SDK's <mega65.h>: `ETHERNET` is a volatile
 * struct __45E100 at $D6E0, and the ETH_*_MASK constants beside it are
 * generated from mega65-core's iomap.txt, so they agree with the hardware by
 * construction.  Only what that generated set has no name for is added here.
 *
 * This file is MIT OR Apache-2.0 so it can be lifted into mos-hardware or
 * mega65-libc later; the browser around it is GPL-3 like the rest of the
 * repository. */

constexpr uint16_t ETH_MAX_FRAME = 1500;
/* Ethernet's own minimum, which a short frame is padded up to. */
constexpr uint16_t ETH_MIN_FRAME = 60;
/* The check sequence, which the length of a *received* frame includes and the
 * length of a transmitted one does not: an ARP exchange is 42 bytes of
 * content, goes out padded to 60, and comes back reported as 64.  A buffer
 * sized to the wire minimum therefore receives nothing, because eth_receive()
 * drops what will not fit. */
constexpr uint16_t ETH_FCS_BYTES = 4;
constexpr uint16_t ETH_MIN_RECEIVED = ETH_MIN_FRAME + ETH_FCS_BYTES;

/* The frame header every protocol above sits behind.  Everything on the wire
 * is big-endian, which is the opposite of this CPU, so the two-byte fields are
 * assembled a byte at a time rather than through a cast. */
constexpr uint8_t MAC_BYTES = 6;
constexpr uint8_t ETH_DESTINATION = 0;
constexpr uint8_t ETH_SOURCE = 6;
constexpr uint8_t ETH_TYPE = 12;
constexpr uint8_t ETH_HEADER_BYTES = 14;

/* Sets the receive filter, and nothing else -- see eth.c for why this must not
 * reset the controller.  Interrupts are already masked by the time a freezer
 * tool runs, which is what makes polling safe. */
void eth_init(void);

/* The controller's own address, six bytes, which is this machine's name on
 * the wire. */
void eth_mac(uint8_t* out);

/* Whether the transmitter has finished with the last frame it was given. */
[[nodiscard]] bool eth_tx_idle(void);

/* Copies `length` bytes to the transmit buffer and starts the transmitter. */
void eth_send(const uint8_t* frame, uint16_t length);

/* How many bytes of the waiting frame are now in `into`, and 0 when no frame
 * waits, when one arrived with a failed CRC, or when one was longer than
 * `limit`.
 *
 * All three are "no usable frame", and the over-long one is dropped rather
 * than delivered in part: a partial frame parses as a plausible short one,
 * where nothing at all is unmistakable.  So what comes back is always a whole
 * frame a parser may walk, and `limit` is also the cheap way to refuse a frame
 * before it is copied -- the receiver runs promiscuous, so most of what
 * arrives is somebody else's. */
[[nodiscard]] uint16_t eth_receive(uint8_t* into, uint16_t limit);
