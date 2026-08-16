// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* The 45E100 itself: the receive filter, the two buffers the controller
 * offers a page at a time, and the DMA that moves a frame across. */

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
 * flag nibble below, but it arrives with the frame and so saves nothing; the
 * cheap refusal is eth_receive()'s `limit`. */
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

#ifdef ETH_COUNTERS
/* The controller's debug window, which the SDK's fifteen-byte struct stops one
 * byte short of -- named here rather than reached by indexing past the end of
 * that struct, which would silently land elsewhere if a member were ever added.
 *
 * Not mega65_regs.h's REG8(), which is the same cast and exists for exactly
 * this: that header is GPL-3 and this file is MIT OR Apache-2.0 so it can be
 * lifted into mos-hardware or mega65-libc, which including it would prevent.
 *
 * What it reports is whatever was last written to it: 0 selects the buffer
 * identities, of which the low two bits are the buffer the CPU reads through
 * (ethernet.vhdl:1441-1443).  A value with both top bits set would set the
 * preamble length instead (1870-1874), which 0 is not. */
#define ETH_DEBUG (*(volatile uint8_t*)0xD6EF)
static constexpr uint8_t ETH_DEBUG_BUFFERS = 0x00;
static constexpr uint8_t ETH_CPU_BUFFER_MASK = 0b00000011;
#define ETH_CPU_BUFFER (ETH_DEBUG & ETH_CPU_BUFFER_MASK)
#endif

void eth_init(void) {
    /* Setting the filter is the whole of it.  Deliberately no reset: the reset
     * lines are active low, so taking the controller down and bringing it back
     * needs a settling delay either side -- the best part of a second -- and
     * doing it without one leaves the controller down and nothing arrives.
     * There is also nothing to reset for.  The machine has been receiving
     * frames all along, and a freezer tool is a guest on a running controller
     * rather than the thing that starts it. */
    ETHERNET.ctrl3 = ETH_FILTER;
#ifdef ETH_COUNTERS
    /* Asked for explicitly rather than assumed: a freezer tool inherits a
     * controller something else has been using, and this register reports
     * whatever that left selected. */
    ETH_DEBUG = ETH_DEBUG_BUFFERS;
#endif
}

void eth_mac(uint8_t* out) {
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        out[i] = ETHERNET.macaddr[i];
    }
}

bool eth_tx_idle(void) {
    return (ETHERNET.ctrl1 & ETH_TXIDLE_MASK) != 0;
}

#ifdef ETH_COUNTERS
/* Whether the bounded wait below ever runs out, which is the question these
 * answer: a frame written over one still going out is a frame the wire never
 * saw, and a lost acknowledgement mid-transfer is indistinguishable from the
 * server never having been answered.  `eth_sends` is the denominator. */
uint32_t eth_sends = 0;
uint32_t eth_tx_busy = 0;

/* And whether the rotation below actually happens, which is the same shape of
 * question at the other end.  The controller declines to advance the CPU's
 * buffer when the ethernet side is filling the one it would advance into --
 * ethernet.vhdl:1485-1503, the "safety straps" -- and reports the refusal
 * nowhere eth_receive() looks, so a refused rotation reads as the frame just
 * consumed arriving a second time.  `eth_rx_rotates` is the denominator.
 *
 * The two are counted apart because they want different answers: the window is
 * registered and so lags the write by a cycle or more (ethernet.vhdl:1508),
 * which waiting cures, while a refusal is a rotation that does not happen at
 * all and which the receive path has to notice instead. */
uint32_t eth_rx_rotates = 0;
uint32_t eth_rx_late = 0;
uint32_t eth_rx_norotate = 0;

#define COUNT_ETH(counter) ((counter)++)
#else
#define COUNT_ETH(counter) ((void)0)
#endif

void eth_send(const uint8_t* frame, uint16_t length) {
    /* A bound, not a wait: the longest frame this sends clocks out in about
     * 46 microseconds and this covers 71, while a longer ceiling would block
     * the receive queue -- three frames deep -- for milliseconds. */
    static constexpr uint8_t TX_IDLE_LIMIT = 255;

    /* The transmit buffer is one buffer: writing a second frame into it while
     * the first is still going out replaces a frame the wire never saw, and
     * nothing downstream can tell.  Bounded, so a controller that never
     * reports idle costs a frame rather than the machine. */
    uint8_t spin = 0;
    for (; spin < TX_IDLE_LIMIT && !eth_tx_idle(); spin++) {
    }
    COUNT_ETH(eth_sends);
    if (spin >= TX_IDLE_LIMIT) {
        COUNT_ETH(eth_tx_busy);
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

#ifdef ETH_COUNTERS
    const uint8_t before = ETH_CPU_BUFFER;
#endif

    /* Rotating is what brings the frame into the window, so it comes before
     * the read rather than after it.  Two writes, for the 0 -> 1 edge. */
    ETHERNET.ctrl2 = 0;
    ETHERNET.ctrl2 = ETH_RX_ROTATE;

#ifdef ETH_COUNTERS
    /* Watched, not waited for.  A spin here would give the window time the
     * shipping build never gives it, so the instrumented build would stop
     * seeing the very lag it was built to find; the second look below is taken
     * after two far reads this function performs anyway, which costs the
     * measurement nothing and the timing nothing. */
    COUNT_ETH(eth_rx_rotates);
    const bool rotated = ETH_CPU_BUFFER != before;
    if (!rotated) {
        COUNT_ETH(eth_rx_late);
    }
#endif

    const uint8_t low = lpeek(ETH_BUFFER);
    const uint8_t high = lpeek(ETH_BUFFER + 1);

#ifdef ETH_COUNTERS
    /* Still where it was after all that: not slow to settle but refused, the
     * controller having declined to advance into a buffer the ethernet side
     * holds (ethernet.vhdl:1485-1503). */
    if (!rotated && ETH_CPU_BUFFER == before) {
        COUNT_ETH(eth_rx_norotate);
    }
#endif

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
