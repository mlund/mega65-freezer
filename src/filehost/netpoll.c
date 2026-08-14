// SPDX-License-Identifier: MIT OR Apache-2.0
#include "netpoll.h"

#include "arp.h"
#include "eth.h"

/* Internal linkage, per the repo rule on file-scope data. */
static uint16_t answered;

uint16_t net_answers(void) {
    return answered;
}

uint16_t net_poll(uint8_t* frame, uint16_t limit, const struct NetEndpoint* us) {
    const uint16_t got = eth_receive(frame, limit);
    if (!got) {
        return 0;
    }
    /* Answered in the buffer it arrived in, and then gone: a caller above has
     * nothing to do with a question about its own address.  arp_answer()
     * refuses while `us` has no address yet, so a client still taking a lease
     * needs no special case here. */
    const uint16_t answer = arp_answer(frame, got, us->mac, us->ip);
    if (answer) {
        eth_send(frame, answer);
        answered++;
        return 0;
    }
    return got;
}
