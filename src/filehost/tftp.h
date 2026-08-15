// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include "eth.h" /* ETH_FCS_BYTES */
#include "ip.h"
#include "net.h"

#include <stdbool.h>
#include <stdint.h>

/* TFTP, RFC 1350: reading one file from a server that need not know what a
 * MEGA65 is.
 *
 * A whole frame goes in and a whole frame comes out, as in dhcp.h.  Which
 * message answers which, the port the server moves the transfer to, the block
 * numbering that decides what is new and what is a repeat, and the UDP and
 * IPv4 headers are all fixed by the RFC, so a caller asked to supply them could
 * supply only one answer.  What is genuinely the caller's is the clock, where
 * the bytes go, and who the server is -- an address it leased or was told, and
 * the hardware address it resolved for that address or for the router in front
 * of it.
 *
 * Frames are built and read, never sent, so this is tested on the host,
 * sequencing included. */

/* One block, which is one SD sector: the reason 512 is left alone rather than
 * negotiated up to a whole frame's worth. */
constexpr uint16_t TFTP_BLOCK_BYTES = 512;
/* Where a block's bytes sit in the frame they arrived in, past the opcode and
 * block number.  A transfer never copies them anywhere else. */
constexpr uint8_t TFTP_DATA_AT = UDP_PAYLOAD_AT + 4;
/* What the receive buffer must hold, and the limit worth passing net_poll():
 * the receiver is promiscuous, so a limit that stops at the largest frame this
 * transfer can be refuses the rest before the DMA rather than after.  The check
 * sequence is in the count because a received length includes it (eth.h) and a
 * buffer one sector wide would therefore drop every full block. */
constexpr uint16_t TFTP_RECEIVE_BYTES = TFTP_DATA_AT + TFTP_BLOCK_BYTES + ETH_FCS_BYTES;

/* The longest name that can be asked for, which is the catalogue's path field
 * (ether65 docs/FILEHOST.md §2): a client asks for `catalog` and for the paths
 * the catalogue named, and nothing else exists to ask for.  A longer one is
 * refused rather than truncated -- a truncated path names a different file. */
constexpr uint8_t TFTP_NAME_MAX = 48;
/* What the send buffer must hold: the request, being the name plus the mode
 * and the one option asked for.  Everything else sent is a four-byte
 * acknowledgement. */
constexpr uint16_t TFTP_SEND_BYTES =
    UDP_PAYLOAD_AT + 2 + (TFTP_NAME_MAX + 1) + sizeof "octet" + sizeof "tsize" + sizeof "0";

/* What a refusal this client made itself carries, in place of a code from the
 * server: outside the codes RFC 1350 §5 gives a server, so the two cannot be
 * confused -- and 0 is one of them, meaning "not defined, see the message". */
constexpr uint16_t TFTP_REFUSED = 0xFFFF;

/* How far the transfer got.  Requesting means the server has said nothing at
 * all, which is a different failure from one that stopped part way.
 *
 * Declared in the order a transfer passes through them, and the two that end it
 * last: "still expecting something" is a comparison against TftpDone, which is
 * 42 bytes cheaper than naming the two stages it stands for. */
enum TftpStage : uint8_t {
    TftpRequesting,
    TftpTransferring,
    TftpDone,
    TftpFailed,
};

/* The transfer in progress.
 *
 * `size` is what the server said the file is, from the tsize option of RFC
 * 2349 -- asked for because room has to be found for a file before its first
 * block is written down, and a server may decline to answer, leaving
 * `has_size` false.  `data_length` is the bytes of the file the last step
 * delivered, at TFTP_DATA_AT in the same buffer that step was given: zero on
 * every step that delivered none, which is a repeat the server sent again, an
 * option acknowledgement, or a timeout.  It answers for the step just made and
 * nothing else: read between steps it still holds the previous answer, so a
 * caller counts where it steps.
 *
 * `error` is the code the server refused with, or TFTP_REFUSED. */
struct TftpClient {
    struct NetEndpoint us;
    struct NetEndpoint server;
    const char* name;
    uint32_t size;
    uint16_t block;
    uint16_t data_length;
    uint16_t error;
    enum TftpStage stage;
    bool has_size;
};

/* Begins a read of `name` from `server`.
 *
 * Both endpoints are addresses only: the ports are this module's, the server's
 * being the reserved one until the server names another and ours being chosen
 * from `seed` so that a late block from a previous transfer is not read as this
 * one's.  Any value the caller has that moves will do.
 *
 * `name` is not copied -- it is re-read on every retransmission, so it must
 * outlive the transfer.  A name too long for TFTP_NAME_MAX, or a machine that
 * has not been given an address of its own yet, leaves the client failed with
 * nothing to send. */
void tftp_start(struct TftpClient* client,
    const struct NetEndpoint* us,
    const struct NetEndpoint* server,
    const char* name,
    uint16_t seed);

/* Cranks the transfer: returns the length of the frame left in `out`, or 0 when
 * there is nothing to send.  `in` is a received frame; `in_length` 0 means a
 * timeout, so say the current step again.  Anything that is not this transfer's
 * next word is dropped here -- another transfer's blocks, a block already
 * taken, the frames of every other machine on the LAN.
 *
 * The whole loop: start, step with nothing, send what comes back; then step
 * with every frame and on every timeout, sending anything non-zero and writing
 * `data_length` bytes from TFTP_DATA_AT wherever they belong, until
 * tftp_done() or tftp_failed().  The step that takes the last block returns the
 * acknowledgement that ends the transfer, so it is sent like any other.
 *
 * Stepping on past tftp_done() answers the last block again if the server sends
 * it again, which is RFC 1350 §6's dallying: a lost final acknowledgement
 * otherwise leaves the server recording a failure for a file that arrived
 * whole.  A caller that stops instead loses nothing but that. */
[[nodiscard]] uint16_t tftp_step(
    struct TftpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out);

/* Whether the whole file arrived. */
[[nodiscard]] static inline bool tftp_done(const struct TftpClient* client) {
    return client->stage == TftpDone;
}

/* Whether the server -- or this client -- refused it. */
[[nodiscard]] static inline bool tftp_failed(const struct TftpClient* client) {
    return client->stage == TftpFailed;
}
