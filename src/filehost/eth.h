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

/* $D6E1.1 means two different things: reading it is part of the free-buffer
 * count the generated ETH_RXBF_MASK names, writing it asks for the next
 * received frame.  The write is edge triggered -- ethernet.vhdl:1787 keeps
 * last_rx_rotate_bit and acts on a 0 -> 1 transition -- so it takes a write
 * with the bit low and then one with it high, not a single write. */
constexpr uint8_t ETH_RX_ROTATE = 0b00000010;

/* Both frame buffers, in the 28-bit space where DMA reaches them.  Reading
 * this address gives the receive buffer and writing it goes to the transmit
 * one, so the same constant serves both and a copy between them is not the
 * no-op it looks like.
 *
 * Not mapped over $D800: that window hides the CIAs at $DC00/$DD00, and it
 * would need the ethernet I/O personality ($45/$54) rather than the standard
 * knock the tools already perform. */
constexpr uint32_t ETH_BUFFER = 0xFFDE800;

/* A received frame is preceded by two bytes, and only twelve of their sixteen
 * bits are the length -- the top four are flags
 * (mega65-user-guide/appendix-45e100-registers.tex, the $D000/$D001 rows).
 * Reading the pair as a plain 16-bit length yields values in the thousands for
 * ordinary frames, which is a mistake that looks like a dead receiver rather
 * than like a decoding error. */
constexpr uint16_t ETH_RX_LENGTH_BYTES = 2;
constexpr uint16_t ETH_RX_LENGTH_MASK = 0x0FFF;
constexpr uint8_t ETH_RX_MULTICAST = 0x10;
constexpr uint8_t ETH_RX_BROADCAST = 0x20;
/* The controller's own answer to "is this addressed to us", which is what
 * makes software filtering cheap while the receiver runs promiscuous. */
constexpr uint8_t ETH_RX_TO_US = 0x40;
constexpr uint8_t ETH_RX_BAD_CRC = 0x80;

constexpr uint16_t ETH_MAX_FRAME = 1500;
/* Ethernet's own minimum, less the CRC the controller appends. */
constexpr uint16_t ETH_MIN_FRAME = 60;

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

void eth_mac(uint8_t* out);
bool eth_tx_idle(void);

/* Copies `length` bytes to the transmit buffer and starts the transmitter. */
void eth_send(const uint8_t* frame, uint16_t length);

/* The frame waiting in the receive buffer, or 0 when there is none and when
 * one arrived with a failed CRC.  Returns the frame's whole length even where
 * that is more than `limit`, so a caller can tell a truncated copy from a
 * short frame -- but never returns a length without having copied what it
 * could, because a caller cannot distinguish a stale buffer from a fresh one.
 *
 * `flags` takes the controller's own verdict on the frame when it is not null:
 * ETH_RX_TO_US and friends. */
uint16_t eth_receive(uint8_t* into, uint16_t limit, uint8_t* flags);
