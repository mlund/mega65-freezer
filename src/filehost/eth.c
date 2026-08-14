// SPDX-License-Identifier: MIT OR Apache-2.0
#include "eth.h"

#include "dma.h"

#include <mega65.h>

/* Everything on the wire, CRC checked, both clock phases at 1.  The phases are
 * two-bit fields rather than flags, hence the shifts.
 *
 * Promiscuous -- ETH_NOPROM_MASK deliberately clear.  With the hardware filter
 * on, measurement showed only broadcast frames arriving and the unicast reply
 * to our own ARP request never reaching us.  iomap.txt does not settle it
 * either: $D6E5.0 is annotated both "disable promiscuous mode" and "enable
 * filtering of unicast frames if MAC address does not match", and $D6E5.5 is
 * annotated as both the multicast and the unicast enable.
 *
 * So every frame on the wire arrives here, and deciding which are ours is the
 * caller's: udp_parse() matches the destination address, and the exchanges
 * that run before there is an address to match -- DHCP -- match on their own
 * transaction id instead.  The controller offers its own verdict in the frame's
 * flag nibble below, but it arrives with the frame and so cannot save the copy;
 * the cheap way to refuse a frame before it is copied is a small `limit`. */
static constexpr uint8_t ETH_PHASE_ONE = 1;
static constexpr uint8_t ETH_FILTER =
    ETH_BCST_MASK | ETH_MCST_MASK | (uint8_t)(ETH_PHASE_ONE << 2) | (uint8_t)(ETH_PHASE_ONE << 6);

/* $D6E1.1 means two different things: reading it is part of the free-buffer
 * count the generated ETH_RXBF_MASK names, writing it asks for the next
 * received frame.  The write is edge triggered -- ethernet.vhdl:1787 keeps
 * last_rx_rotate_bit and acts on a 0 -> 1 transition -- so it takes a write
 * with the bit low and then one with it high, not a single write. */
static constexpr uint8_t ETH_RX_ROTATE = 0b00000010;

/* Both frame buffers, in the 28-bit space where DMA reaches them.  Reading
 * this address gives the receive buffer and writing it goes to the transmit
 * one, so the same constant serves both and a copy between them is not the
 * no-op it looks like.
 *
 * Not mapped over $D800: that window hides the CIAs at $DC00/$DD00, and it
 * would need the ethernet I/O personality ($45/$54) rather than the standard
 * knock the tools already perform. */
static constexpr uint32_t ETH_BUFFER = 0xFFDE800;

/* A received frame is preceded by two bytes, and only twelve of their sixteen
 * bits are the length -- the top four are flags, of which $80 is a failed CRC
 * (mega65-user-guide/appendix-45e100-registers.tex, the $D000/$D001 rows).
 * Reading the pair as a plain 16-bit length yields values in the thousands for
 * ordinary frames, which is a mistake that looks like a dead receiver rather
 * than like a decoding error.  The other three flags -- multicast, broadcast,
 * addressed-to-us -- say nothing the frame's own destination address does not,
 * so nothing here reads them. */
static constexpr uint16_t ETH_RX_LENGTH_BYTES = 2;
static constexpr uint16_t ETH_RX_LENGTH_MASK = 0x0FFF;
static constexpr uint8_t ETH_RX_BAD_CRC = 0x80;

void eth_init(void) {
    /* Setting the filter is the whole of it.  Deliberately no reset: the reset
     * lines are active low, so taking the controller down and bringing it back
     * needs a settling delay either side -- the best part of a second -- and
     * doing it without one leaves the controller down and nothing arrives.
     * There is also nothing to reset for.  The machine has been receiving
     * frames all along, and a freezer tool is a guest on a running controller
     * rather than the thing that starts it. */
    ETHERNET.ctrl3 = ETH_FILTER;
}

void eth_mac(uint8_t* out) {
    for (uint8_t i = 0; i < 6; i++) {
        out[i] = ETHERNET.macaddr[i];
    }
}

bool eth_tx_idle(void) {
    return (ETHERNET.ctrl1 & ETH_TXIDLE_MASK) != 0;
}

void eth_send(const uint8_t* frame, uint16_t length) {
    /* A bound, not a wait: the longest frame this sends clocks out in about
     * 46 microseconds and this covers 71, while a longer ceiling would block
     * the receive queue -- three frames deep -- for milliseconds. */
    static constexpr uint8_t TX_IDLE_LIMIT = 255;

    /* The transmit buffer is one buffer: writing a second frame into it while
     * the first is still going out replaces a frame the wire never saw, and
     * nothing downstream can tell.  Bounded, so a controller that never
     * reports idle costs a frame rather than the machine. */
    for (uint8_t spin = 0; spin < TX_IDLE_LIMIT && !eth_tx_idle(); spin++) {
    }
    lcopy((Addr28)(uint16_t)frame, (Addr28)ETH_BUFFER, length);
    /* Padded in the transmit buffer rather than by the caller, so a short
     * frame does not oblige every caller to own a 60-byte array. */
    if (length < ETH_MIN_FRAME) {
        lfill((Addr28)(ETH_BUFFER + length), 0, ETH_MIN_FRAME - length);
        length = ETH_MIN_FRAME;
    }
    ETHERNET.txsz = length;
    ETHERNET.command = ETHERNET_STARTTX;
}

uint16_t eth_receive(uint8_t* into, uint16_t limit) {
    if (!(ETHERNET.ctrl2 & ETH_RXQ_MASK)) {
        return 0;
    }

    /* Rotating is what brings the frame into the window, so it comes before
     * the read rather than after it.  Two writes, for the 0 -> 1 edge. */
    ETHERNET.ctrl2 = 0;
    ETHERNET.ctrl2 = ETH_RX_ROTATE;

    const uint8_t low = lpeek(ETH_BUFFER);
    const uint8_t high = lpeek(ETH_BUFFER + 1);

    /* A failed CRC means truncated or corrupted in transit, so there is
     * nothing worth copying and nothing worth reporting a length for.  A
     * zero-length frame falls through the same way: lcopy of nothing is
     * nothing (src/dma.c), so it needs no test of its own. */
    if (high & ETH_RX_BAD_CRC) {
        return 0;
    }
    const uint16_t length = (uint16_t)(low | ((uint16_t)high << 8)) & ETH_RX_LENGTH_MASK;
    if (length > limit) {
        return 0;
    }
    lcopy((Addr28)(ETH_BUFFER + ETH_RX_LENGTH_BYTES), (Addr28)(uint16_t)into, length);
    return length;
}
