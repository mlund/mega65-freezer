// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* Where the fields of a TCP header are, and the sequencing that turns one
 * outbound connection into a stream of bytes arriving in order. */

#include "tcp.h"

#include <string.h>

/* Offsets within the TCP header, RFC 793 §3.1. */
static constexpr uint8_t TCP_SOURCE_PORT = 0;
static constexpr uint8_t TCP_DESTINATION_PORT = 2;
static constexpr uint8_t TCP_SEQUENCE = 4;
static constexpr uint8_t TCP_ACKNOWLEDGE = 8;
static constexpr uint8_t TCP_OFFSET = 12;
static constexpr uint8_t TCP_FLAGS = 13;
static constexpr uint8_t TCP_WINDOW = 14;
static constexpr uint8_t TCP_CHECKSUM = 16;
static constexpr uint8_t TCP_URGENT = 18;

/* The control bits, of which this client uses five and needs none of the
 * rest. */
static constexpr uint8_t TCP_FIN = 0x01;
static constexpr uint8_t TCP_SYN = 0x02;
static constexpr uint8_t TCP_RST = 0x04;
static constexpr uint8_t TCP_PSH = 0x08;
static constexpr uint8_t TCP_ACK = 0x10;

/* The one option this sends: kind 2, four bytes, maximum segment size. */
static constexpr uint8_t TCP_OPTION_MSS = 2;

/* The dynamic range of RFC 6335, where a port a client picks for itself
 * belongs. */
static constexpr uint16_t EPHEMERAL_FIRST = 0xC000;

/* Whether `a` is at or after `b` in a sequence space that wraps.
 *
 * The subtraction and not the values: RFC 793 §3.3 numbers a stream modulo
 * 2^32, so 4 is after 0xFFFFFFF0 and comparing the two as integers says the
 * opposite.  Everything after a wrap then reads as an old duplicate, and the
 * transfer stops dead at the boundary -- with no error, because dropping an
 * old duplicate is exactly what the code is meant to do. */
[[nodiscard]] static bool seq_at_least(uint32_t a, uint32_t b) {
    return (uint32_t)(a - b) < 0x80000000UL;
}

/* Whichever segment the current stage owes the server: the SYN until the
 * connection opens, then whatever of the request is unacknowledged, then an
 * acknowledgement on its own -- with our FIN on it once theirs has arrived.
 *
 * Always sent from `send_unacked`, so a server that took only part of the
 * request is sent the rest rather than the whole of it again. */
static uint16_t say(const struct TcpClient* client, uint8_t* out) {
    uint8_t* segment = &out[IPV4_PAYLOAD_AT];
    uint8_t flags;
    uint8_t options = 0;
    uint16_t payload = 0;

    if (client->stage == TcpConnecting) {
        flags = TCP_SYN;
        /* Told rather than left to the server's guess: this receiver has room
         * for exactly one frame, so a server that assumed the path would carry
         * more would find out through a dropped frame instead. */
        segment[TCP_HEADER_BYTES] = TCP_OPTION_MSS;
        segment[TCP_HEADER_BYTES + 1] = TCP_OPTION_BYTES;
        net_put16(&segment[TCP_HEADER_BYTES + 2], TCP_MSS);
        options = TCP_OPTION_BYTES;
    } else {
        flags = TCP_ACK;
        const uint16_t sent = (uint16_t)(client->send_unacked - client->request_seq);
        if (sent < client->request_length) {
            payload = (uint16_t)(client->request_length - sent);
            flags |= TCP_PSH;
            memcpy(&segment[TCP_HEADER_BYTES], &client->request[sent], payload);
        }
        if (client->stage == TcpClosing) {
            flags |= TCP_FIN;
        }
    }

    const uint16_t header = (uint16_t)(TCP_HEADER_BYTES + options);
    const uint16_t length = (uint16_t)(header + payload);
    net_put16(&segment[TCP_SOURCE_PORT], client->us.port);
    net_put16(&segment[TCP_DESTINATION_PORT], client->server.port);
    net_put32(&segment[TCP_SEQUENCE], client->send_unacked);
    /* Zero while the connection is opening, which is what a SYN with no
     * acknowledgement flag means and what receive_next holds until the
     * server's own sequence number is known. */
    net_put32(&segment[TCP_ACKNOWLEDGE], client->receive_next);
    segment[TCP_OFFSET] = (uint8_t)((header / 4) << 4);
    segment[TCP_FLAGS] = flags;
    net_put16(&segment[TCP_WINDOW], TCP_WINDOW_BYTES);
    net_put16(&segment[TCP_CHECKSUM], 0); /* zero while it is computed over */
    net_put16(&segment[TCP_URGENT], 0);

    ipv4_build_header(out, &client->us, &client->server, IPV4_PROTOCOL_TCP, length);
    /* Unlike UDP's, a TCP checksum is not optional and zero does not mean "not
     * computed", so there is no all-ones case to fold. */
    uint16_t sum = ipv4_pseudo_sum(client->us.ip, client->server.ip, IPV4_PROTOCOL_TCP, length);
    sum = ip_sum(sum, segment, length);
    net_put16(&segment[TCP_CHECKSUM], ip_sum_final(sum));

    return (uint16_t)(IPV4_PAYLOAD_AT + length);
}

/* What arrived, and whether it belonged to this connection.  False means
 * nothing is owed in reply. */
static bool take(struct TcpClient* client, const uint8_t* in, uint16_t in_length) {
    struct Datagram datagram;
    if (!ipv4_parse(in, in_length, &client->us, IPV4_PROTOCOL_TCP, &datagram)) {
        return false;
    }
    if (!net_same(datagram.from.ip, client->server.ip, IPV4_BYTES)) {
        return false;
    }
    if (datagram.payload_length < TCP_HEADER_BYTES) {
        return false;
    }
    const uint8_t* segment = &in[IPV4_PAYLOAD_AT];
    if (net_get16(&segment[TCP_SOURCE_PORT]) != client->server.port) {
        return false;
    }
    if (net_get16(&segment[TCP_DESTINATION_PORT]) != client->us.port) {
        return false;
    }

    /* The payload is what IPv4 says the segment is, less what the data offset
     * says the header is.  Not a constant: a Linux server with timestamps on
     * puts twelve bytes of options on every segment, and subtracting a fixed
     * forty would copy those twelve into the file and overstate its length by
     * that much on every segment -- silently, since nothing checks a file
     * arriving from the network against anything. */
    const uint16_t header = (uint16_t)((segment[TCP_OFFSET] >> 4) * 4);
    if (header < TCP_HEADER_BYTES || header > datagram.payload_length) {
        return false;
    }

    /* Verified, and not only the IPv4 header: a corrupted segment taken as
     * good is a corrupted file written down. */
    uint16_t sum = ipv4_pseudo_sum(
        datagram.from.ip, client->us.ip, IPV4_PROTOCOL_TCP, datagram.payload_length);
    sum = ip_sum(sum, segment, datagram.payload_length);
    if (ip_sum_final(sum)) {
        return false;
    }

    const uint32_t sequence = net_get32(&segment[TCP_SEQUENCE]);
    const uint32_t acknowledged = net_get32(&segment[TCP_ACKNOWLEDGE]);
    const uint8_t flags = segment[TCP_FLAGS];

    if (flags & TCP_RST) {
        client->stage = TcpFailed;
        client->heard = true;
        return false;
    }

    if (client->stage == TcpConnecting) {
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK)) {
            return false;
        }
        /* Our SYN consumed one sequence number, unconditionally and here
         * alone.  WeeIP spends it in two conditional places instead
         * (nwk.c:436-437, on the flags being sent rather than on what was
         * acknowledged); two rules for one increment is one too many, and a
         * sequence number out by one puts every byte after it out by one. */
        if (acknowledged != client->send_next) {
            return false;
        }
        client->send_unacked = acknowledged;
        client->request_seq = acknowledged;
        client->send_next = acknowledged + client->request_length;
        client->receive_next = sequence + 1; /* and so did theirs */
        client->stage = TcpOpen;
        client->heard = true;
        return true;
    }

    /* Past the handshake every segment carries an acknowledgement, and one of
     * something never sent belongs to a connection that is not this one. */
    if (!(flags & TCP_ACK)) {
        return false;
    }
    if (!seq_at_least(client->send_next, acknowledged)) {
        return false;
    }
    if (seq_at_least(acknowledged, client->send_unacked)) {
        client->send_unacked = acknowledged;
    }

    const uint16_t payload = (uint16_t)(datagram.payload_length - header);
    if (payload) {
        if (sequence == client->receive_next) {
            client->data_at = (uint16_t)(IPV4_PAYLOAD_AT + header);
            client->data_length = payload;
            client->receive_next += payload;
        } else {
            /* Nothing here reassembles, so a segment from further along the
             * stream is thrown away and the server asked again by the
             * acknowledgement below.  Counted because the count is what
             * decides whether reassembly has to be written at all. */
#ifdef ETH_COUNTERS
            client->dropped++;
#endif
        }
    }
    /* A FIN takes one sequence number of its own, after whatever it carried,
     * and only once: a FIN sent again because our acknowledgement was lost
     * must be answered without ending the stream a second time.  One beyond a
     * gap is not taken at all -- that would end the body at a hole.
     *
     * Being still Open is what "not taken yet" means, so there is no second
     * flag to keep in step with the stage. */
    if ((flags & TCP_FIN) && client->stage == TcpOpen
        && (uint32_t)(sequence + payload) == client->receive_next) {
        client->receive_next++;
        client->stage = TcpClosing;
        client->send_next++; /* ours, which say() puts on the next segment */
    }
    if (client->stage == TcpClosing && client->send_unacked == client->send_next) {
        client->stage = TcpDone;
    }
    client->heard = true;
    return true;
}

void tcp_start(struct TcpClient* client,
    const struct NetEndpoint* us,
    const struct NetEndpoint* server,
    const uint8_t* request,
    uint16_t request_length,
    uint16_t seed) {
    *client = (struct TcpClient){0};
    client->us = *us;
    client->server = *server;
    client->request = request;
    client->request_length = request_length;
    client->us.port = (uint16_t)(EPHEMERAL_FIRST | seed);
    /* RFC 6528 wants an initial sequence number a stranger cannot guess; a
     * seed that moves is all this machine has, and what it is really for is
     * that a segment left over from the last connection does not fall inside
     * this one's window.
     *
     * The seed in both halves rather than only the top one, which costs
     * nothing and makes the whole space reachable: with the low half always
     * zero no seed can put a connection near the wrap, so the arithmetic that
     * handles it would never run outside a test that reached into the struct
     * to arrange it. */
    client->send_unacked = ((uint32_t)seed << 16) | seed;
    client->send_next = client->send_unacked + 1; /* the SYN's own number */
    /* TCP has no reserved port, so one not named is a caller's mistake rather
     * than something to guess at; and a segment from 0.0.0.0 has nowhere to be
     * answered to. */
    if (!server->port || net_zero(us->ip, IPV4_BYTES)) {
        client->stage = TcpFailed;
    }
}

uint16_t tcp_step(struct TcpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out) {
    client->data_length = 0;
    client->heard = false;
    if (client->stage >= TcpDone) {
        return 0;
    }
    if (!in_length) {
        /* The first tick is what puts the SYN on the wire; every one after it
         * says something again, which is what the counter is for. */
#ifdef ETH_COUNTERS
        if (client->spoke) {
            client->retransmits++;
        }
        client->spoke = true;
#endif
        return say(client, out);
    }
    if (!take(client, in, in_length)) {
        return 0;
    }
    if (client->stage >= TcpDone) {
        return 0;
    }
#ifdef ETH_COUNTERS
    client->spoke = true;
#endif
    return say(client, out);
}
