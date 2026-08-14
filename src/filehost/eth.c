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
 * annotated as both the multicast and the unicast enable.  So the address
 * check is done in software, where what it does is written down. */
static constexpr uint8_t ETH_PHASE_ONE = 1;
static constexpr uint8_t ETH_FILTER =
    ETH_BCST_MASK | ETH_MCST_MASK | (uint8_t)(ETH_PHASE_ONE << 2) | (uint8_t)(ETH_PHASE_ONE << 6);

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

uint16_t eth_receive(uint8_t* into, uint16_t limit, uint8_t* flags) {
    if (!(ETHERNET.ctrl2 & ETH_RXQ_MASK)) {
        return 0;
    }

    /* Rotating is what brings the frame into the window, so it comes before
     * the read rather than after it.  Two writes, for the 0 -> 1 edge. */
    ETHERNET.ctrl2 = 0;
    ETHERNET.ctrl2 = ETH_RX_ROTATE;

    const uint8_t low = lpeek(ETH_BUFFER);
    const uint8_t high = lpeek(ETH_BUFFER + 1);
    if (flags) {
        *flags = high;
    }
    /* Truncated or corrupted in transit, so there is nothing worth copying. */
    if (high & ETH_RX_BAD_CRC) {
        return 0;
    }

    uint16_t length = (uint16_t)(low | ((uint16_t)high << 8)) & ETH_RX_LENGTH_MASK;
    if (!length) {
        return 0;
    }
    lcopy((Addr28)(ETH_BUFFER + ETH_RX_LENGTH_BYTES),
        (Addr28)(uint16_t)into,
        length < limit ? length : limit);
    return length;
}
