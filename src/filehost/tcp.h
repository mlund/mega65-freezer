// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include "eth.h"
#include "ip.h"
#include "net.h"

#include <stdbool.h>
#include <stdint.h>

/* One outbound TCP connection: enough of RFC 793 to ask a web server a
 * question and read the answer, and no more.  No listen, no second
 * connection, no scheduler, no TIME_WAIT.
 *
 * http.h is the module; this is how it is built, and it is a header rather
 * than a static block only so its own tests can drive it.  Nothing but http.c
 * includes it -- a public TCP interface with one user would be a seam invented
 * for a caller that does not exist.
 *
 * A whole frame goes in and a whole frame comes out, so this compiles for the
 * host and the sequencing is tested there.  That matters more
 * here than for TFTP: sequence arithmetic is wrong only in the cases nobody
 * arranges by hand -- a wrap, an option nobody expected, a segment arriving
 * twice. */

constexpr uint8_t TCP_HEADER_BYTES = 20;
/* Where a segment's payload sits when the header carries no options.  A header
 * that does carry them is followed rather than refused -- unlike IPv4's, TCP
 * options are ordinary: any Linux server with timestamps on sends twelve bytes
 * of them on every segment. */
constexpr uint8_t TCP_PAYLOAD_AT = IPV4_PAYLOAD_AT + TCP_HEADER_BYTES;

/* The largest payload we tell the server a segment may carry.  Chosen so the
 * whole frame is ethernet's largest: 1460 + the two 20-byte headers + the
 * 14-byte ethernet one is 1514, and 1518 once received with its check
 * sequence. */
constexpr uint16_t TCP_MSS = 1460;
static_assert(TCP_PAYLOAD_AT + TCP_MSS + ETH_FCS_BYTES == ETH_MAX_RECEIVED,
    "the advertised segment must be exactly what a full frame carries");

/* How much the server may have in flight before it must wait for us.
 *
 * Three segments, because three is what the controller can hold: it has four
 * receive buffers (mega65-core src/vhdl/ethernet.vhdl:45, threaded down from
 * machine.vhdl:59 and never overridden), one of which the CPU side holds, so
 * eth_rx_buffers_free counts up to three and the $D6E1.1-2 field saturates
 * there.  A window wider than that is not throughput, it is frames dropped and
 * sent again.
 *
 * Throughput is the window divided by the round trip, so this is the number
 * that sets it: measured against the live proxy at 23ms, two segments gave
 * 86 KB/s and three give 122. */
constexpr uint16_t TCP_WINDOW_BYTES = 3 * TCP_MSS;

/* What the send buffer must hold beyond the request: the SYN carries a
 * maximum-segment-size option and nothing else ever does. */
constexpr uint8_t TCP_OPTION_BYTES = 4;

/* How far the connection got.  Connecting means the server has said nothing at
 * all, which is a different failure from one that stopped part way; Closing is
 * its FIN taken and ours outstanding.
 *
 * In the order a connection passes through them, the two that end it last, so
 * "still expecting something" is one comparison against TcpDone. */
enum TcpStage : uint8_t {
    TcpConnecting,
    TcpOpen,
    TcpClosing,
    TcpDone,
    TcpFailed,
};

/* The connection.
 *
 * `data_at` and `data_length` describe the payload the last step delivered,
 * in the same buffer that step was given -- never copied out of it.  Both
 * answer for the step just made and nothing else: read between steps they
 * still hold the previous answer.
 *
 * `dropped` counts segments whose payload fell outside the window and was
 * thrown away.  It is the number that decides whether receiving strictly in
 * order is good enough or whether reassembly has to be written, so it is a
 * field rather than a debug counter.  `retransmits` counts the steps where a
 * clock tick made us say the same thing again.
 *
 * The sequence numbers are the connection: `send_unacked` is the oldest byte
 * of ours the server has not acknowledged and is where every segment we send
 * starts, `send_next` is one past everything we have to send, and
 * `receive_next` is the sequence number we expect from the server next. */
struct TcpClient {
    struct NetEndpoint us;
    struct NetEndpoint server;
    const uint8_t* request;
    uint32_t send_unacked;
    uint32_t send_next;
    uint32_t request_seq;
    uint32_t receive_next;
    uint16_t request_length;
    uint16_t data_at;
    uint16_t data_length;
#ifdef ETH_COUNTERS
    uint16_t dropped;
    uint16_t retransmits;
#endif
    enum TcpStage stage;
    bool heard;
    /* Whether anything has been sent yet, which is the difference between the
     * tick that opens the connection and every tick after it. */
#ifdef ETH_COUNTERS
    bool spoke;
#endif
};

/* Opens a connection to `server` and asks it `request`.
 *
 * Two entry points and not three: the obvious shape has a tcp_write() beside
 * tcp_step(), but the only thing ever written is the request and it cannot go
 * until the handshake completes.  Handing the request over here and letting
 * this send it when the connection opens means the caller never sees the
 * handshake at all -- the three-way exchange, the retry, and the moment the
 * connection became writable all stay in here.
 *
 * `request` is not copied.  It is re-read on every retransmission, so it must
 * outlive the connection.
 *
 * Our own port and the initial sequence number both come from `seed`, so that
 * a segment left over from a previous connection is not read as this one's;
 * any value the caller has that moves will do.  `server->port` must name a
 * port: TCP has no reserved one, and which port a request belongs at is the
 * protocol above's question.  Without it, or without an address of our own,
 * the connection is failed with nothing to send. */
void tcp_start(struct TcpClient* client,
    const struct NetEndpoint* us,
    const struct NetEndpoint* server,
    const uint8_t* request,
    uint16_t request_length,
    uint16_t seed);

/* Cranks the connection: returns the length of the frame left in `out`, or 0
 * when there is nothing to send.  `in` is a received frame; `in_length` 0
 * means the clock ticked, so say the current step again.  Anything that is not
 * this connection's is dropped here.
 *
 * Every segment the server sends is answered, and answered at once: with a
 * window this small a delayed acknowledgement delays the window update with
 * it, and the server then waits on us for no reason. */
[[nodiscard]] uint16_t tcp_step(
    struct TcpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out);

/* Whether the last frame belonged to this connection -- what a caller times
 * silence against, rather than against bytes delivered, since a segment we are
 * right to drop still proves the server is there. */
[[nodiscard]] static inline bool tcp_heard(const struct TcpClient* client) {
    return client->heard;
}

/* Whether the connection closed cleanly, both FINs exchanged and acknowledged.
 * The body of a reply with no stated length ends here and nowhere else. */
[[nodiscard]] static inline bool tcp_done(const struct TcpClient* client) {
    return client->stage == TcpDone;
}

/* Whether it was reset, refused, or never answered. */
[[nodiscard]] static inline bool tcp_failed(const struct TcpClient* client) {
    return client->stage == TcpFailed;
}
