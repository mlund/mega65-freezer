// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* The messages RFC 1350 fixes, and the sequencing that turns them into a file
 * arriving one block at a time. */

#include "tftp.h"

/* The opcodes, RFC 1350 §5 and RFC 2347.  Two bytes on the wire, and all of
 * them small. */
static constexpr uint8_t TFTP_READ_REQUEST = 1;
static constexpr uint8_t TFTP_DATA = 3;
static constexpr uint8_t TFTP_ACKNOWLEDGE = 4;
static constexpr uint8_t TFTP_ERROR = 5;
static constexpr uint8_t TFTP_OPTION_ACK = 6;

/* An opcode, and the block number or error code behind it. */
static constexpr uint8_t TFTP_OPCODE_BYTES = 2;
static constexpr uint8_t TFTP_HEADER_BYTES = 4;

/* The dynamic range of RFC 6335, which is where a port a client picks for
 * itself belongs.  Seeded with anything that moves, the whole range is
 * reachable and two runs a moment apart do not collide. */
static constexpr uint16_t EPHEMERAL_FIRST = 0xC000;

/* Appends a NUL-terminated string, and says where the next one goes. */
static uint8_t* put_string(uint8_t* at, const char* text) {
    while (*text) {
        *at++ = (uint8_t)*text++;
    }
    *at++ = 0;
    return at;
}

/* Whether the whole of `name` fits a request, terminator included. */
static bool name_fits(const char* name) {
    for (uint8_t i = 0; i <= TFTP_NAME_MAX; i++) {
        if (!name[i]) {
            return true;
        }
    }
    return false;
}

/* Where the string starting at `at` ends, or 0 when the payload runs out before
 * its terminator -- so no option name or value is read past the frame. */
static uint16_t after_string(const uint8_t* payload, uint16_t at, uint16_t length) {
    while (at < length) {
        if (!payload[at]) {
            return (uint16_t)(at + 1);
        }
        at++;
    }
    return 0;
}

/* Whether the option name at `at` is `name`, which is given in lower case.
 * RFC 2347 makes the names case-insensitive, and a server is free to echo one
 * back in a case of its own; letters differ only in bit 5, and an option name
 * is letters. */
static bool option_is(const uint8_t* at, const char* name) {
    for (; *name; name++, at++) {
        if ((*at | 0x20) != *name) {
            return false;
        }
    }
    return !*at;
}

/* The unsigned decimal an option's value is.  The terminator after_string()
 * found is what stops it. */
static uint32_t decimal(const uint8_t* at) {
    uint32_t value = 0;
    for (; *at >= '0' && *at <= '9'; at++) {
        value = value * 10 + (uint8_t)(*at - '0');
    }
    return value;
}

/* Reads what the server granted.  Only tsize was asked for, so only tsize is
 * looked for -- an option nobody asked about is a server's business, not
 * something to act on. */
static void take_options(struct TftpClient* client, const uint8_t* payload, uint16_t length) {
    uint16_t at = TFTP_OPCODE_BYTES;
    for (;;) {
        const uint16_t name = at;
        at = after_string(payload, at, length);
        if (!at) {
            return;
        }
        const uint16_t value = at;
        at = after_string(payload, at, length);
        if (!at) {
            return;
        }
        if (option_is(&payload[name], "tsize")) {
            client->size = decimal(&payload[value]);
            client->has_size = true;
        }
    }
}

/* What arrived, and whether it moved the transfer far enough to be worth
 * answering.
 *
 * Kept out of line although it has one caller: inlined, its live state and
 * tftp_step()'s compete for the same registers, and the spills cost 64 bytes
 * measured against the transfer loop in test/ethtest.c.  The same treatment
 * applied to say() or take_options() costs bytes rather than saving them, so
 * this is the one place it is worth overriding the inliner. */
static __attribute__((noinline)) bool take(
    struct TftpClient* client, const uint8_t* in, uint16_t in_length) {
    struct UdpDatagram datagram;
    if (!udp_parse(in, in_length, &client->us, &datagram)) {
        return false;
    }
    if (!net_same(datagram.from.ip, client->server.ip, IPV4_BYTES)) {
        return false;
    }
    /* Until the server has answered, the port is the reserved one it was asked
     * at; from the first message accepted it is the one that server chose for
     * this transfer, and anything from another port is another transfer's. */
    if (client->stage != TftpRequesting && datagram.from.port != client->server.port) {
        return false;
    }
    /* Every message worth reading here carries an opcode and a block number or
     * an error code behind it, and both are read below without asking again. */
    if (datagram.payload_length < TFTP_HEADER_BYTES) {
        return false;
    }

    /* Whether the transfer still has anything to hear.  Once it is over, one
     * stray frame from the transfer's own port would otherwise carry the file
     * past its end or undo a refusal, so everything that moves the transfer on
     * is refused from here -- everything, that is, but the repeat below. */
    const bool expecting = client->stage < TftpDone;

    const uint8_t* payload = &in[UDP_PAYLOAD_AT];
    switch (net_get16(payload)) {
        case TFTP_OPTION_ACK:
            /* Acknowledged as block zero, which is what starts the transfer.
             * One arriving after a block has been taken is a stray: the
             * options were settled before the first block. */
            if (!expecting || client->block) {
                return false;
            }
            take_options(client, payload, datagram.payload_length);
            client->stage = TftpTransferring;
            break;
        case TFTP_DATA: {
            const uint16_t block = net_get16(&payload[TFTP_OPCODE_BYTES]);
            const uint16_t bytes = datagram.payload_length - TFTP_HEADER_BYTES;
            /* No larger block was negotiated, so a server sending one is not
             * one whose blocks fit where the caller is putting them. */
            if (bytes > TFTP_BLOCK_BYTES) {
                return false;
            }
            /* Where a block has been taken that the server may not know
             * arrived -- the one repeat worth answering, since what was lost
             * was the acknowledgement.  After the last block that is RFC 1350
             * §6's dallying; a refused transfer answers nothing at all. */
            const bool taken = client->stage == TftpTransferring || client->stage == TftpDone;
            if (expecting && block == (uint16_t)(client->block + 1)) {
                client->block = block;
                client->data_length = bytes;
                /* Short of a whole block is the last of them, RFC 1350 §1; a
                 * file that is an exact multiple ends with an empty one. */
                client->stage = bytes < TFTP_BLOCK_BYTES ? TftpDone : TftpTransferring;
            } else if (!taken || block != client->block) {
                /* Anything else is dropped: acknowledging an older block asks
                 * for the file from there again. */
                return false;
            }
            break;
        }
        case TFTP_ERROR:
            if (!expecting) {
                return false;
            }
            client->error = net_get16(&payload[TFTP_OPCODE_BYTES]);
            client->stage = TftpFailed;
            return false;
        default:
            return false;
    }

    /* The transfer's port: this assignment for the first message accepted, and
     * a copy of itself for the rest, which the test above has already matched. */
    client->server.port = datagram.from.port;
    return true;
}

/* Whichever message the current stage owes the server: the request until one is
 * answered, and after that an acknowledgement of the last block taken -- block
 * zero being what acknowledges the options. */
static uint16_t say(const struct TftpClient* client, uint8_t* out) {
    /* Built where it will be sent from, so udp_build() copies the payload onto
     * itself rather than out of a second buffer. */
    uint8_t* payload = &out[UDP_PAYLOAD_AT];
    uint8_t* at = payload;
    if (client->stage == TftpRequesting) {
        net_put16(at, TFTP_READ_REQUEST);
        at = put_string(at + TFTP_OPCODE_BYTES, client->name);
        /* Bytes, not text: netascii would rewrite line endings in a .d81. */
        at = put_string(at, "octet");
        /* How big the file is, which is how much room has to be found before
         * the first block is written down.  A server may decline to say. */
        at = put_string(at, "tsize");
        at = put_string(at, "0");
    } else {
        net_put16(at, TFTP_ACKNOWLEDGE);
        net_put16(at + TFTP_OPCODE_BYTES, client->block);
        at += TFTP_HEADER_BYTES;
    }
    return udp_build(out, &client->us, &client->server, payload, (uint16_t)(at - payload));
}

void tftp_start(struct TftpClient* client,
    const struct NetEndpoint* us,
    const struct NetEndpoint* server,
    const char* name,
    uint16_t seed) {
    /* Zeroed and then filled rather than built as one literal: a literal with
     * two struct members in it is assembled in a temporary and copied over,
     * which measured 34 bytes for the same result. */
    *client = (struct TftpClient){0};
    client->us = *us;
    client->server = *server;
    client->name = name;
    client->us.port = (uint16_t)(EPHEMERAL_FIRST | seed);
    if (!client->server.port) {
        client->server.port = TFTP_PORT;
    }
    /* An address of our own is as necessary as a name that fits: udp_parse()
     * reads an all-zero one as "accept any destination", which is what a
     * machine still asking for a lease needs and the opposite of what a
     * transfer needs -- and a request sent from 0.0.0.0 has nowhere to be
     * answered to anyway. */
    if (!name_fits(name) || net_zero(us->ip, IPV4_BYTES)) {
        client->stage = TftpFailed;
        client->error = TFTP_REFUSED;
    }
}

uint16_t tftp_step(struct TftpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out) {
    client->data_length = 0;
    /* A timeout says the current step again; anything that arrives is answered
     * only when it moved the transfer on. */
    const bool owed =
        in_length ? take(client, in, in_length) : !(tftp_done(client) || tftp_failed(client));
    return owed ? say(client, out) : 0;
}
