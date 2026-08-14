// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "net.h"

#include <stdint.h>

/* Taking frames off the wire, for a loop with an address to defend.
 *
 * Its own header, above arp.h and eth.h, because net.h is the leaf every pure
 * layer includes and this is the one function in the stack that touches
 * hardware -- declaring it there would put an unlinkable symbol in front of
 * every host-tested translation unit. */

/* The next frame that is the caller's to deal with, or 0.
 *
 * "Who has `us`" is answered here rather than handed back.  Anything holding a
 * datagram for this machine asks that first, so a loop that goes silent has its
 * replies quietly undelivered for as long as it runs -- answering where frames
 * arrive leaves nowhere to forget it.  A loop with no address of its own is the
 * exception and may read eth_receive() directly; arp_answer() refuses for
 * 0.0.0.0, so there is nothing to answer.
 *
 * `limit` is eth_receive()'s and means the same. */
[[nodiscard]] uint16_t net_poll(uint8_t* frame, uint16_t limit, const struct NetEndpoint* us);

/* How many questions net_poll() has answered.  Answering is invisible to a
 * caller by design, so this is the only way a test can tell a responder that
 * works from one that never runs. */
[[nodiscard]] uint16_t net_answers(void);
