// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Fetching a named file off the network, in one call.
 *
 * Everything underneath is sans-io and tested on the host; this is where those
 * layers meet the wire, and so the one part of the stack that can only be tried
 * on hardware.  A caller is given an answer and a length rather than a state
 * machine: taking a lease, finding whose hardware address carries the frames,
 * which frame is ours, when to say a thing again and how long to wait each have
 * one sensible answer, and no caller would choose differently.
 *
 * The lease is taken once and kept, so a second fetch is only a transfer. */

/* How it went.  Told apart because each is a different thing to do about it:
 * join a network, name a server, start the one named, ask for something else,
 * or try again. */
enum FetchResult : uint8_t {
    FetchOk,
    FetchNoLease,  /* nothing answered, so this machine has no address */
    FetchNoServer, /* no server address: the lease named none and none was told */
    FetchNoAnswer, /* an address, but nobody on the wire holds it */
    FetchRefused,  /* the server said no; fetch_error() is its code */
    FetchLost,     /* it stopped part way, and what was written is incomplete */
    FetchTooBig,   /* more than the caller offered room for, refused before writing */
};

/* The TFTP server to use when a lease names none, which is the ordinary case:
 * a household router hands out addresses and has never heard of RFC 5859's
 * option 150.  All zeros, until something says otherwise.
 *
 * `port` 0 means TFTP's own, which is what a bare address means; a gateway put
 * somewhere else is named with the port it was put on, since one above 1024
 * needs no root to run. */
void fetch_set_server(const uint8_t* ip, uint16_t port);

/* The server a fetch would go to, and through `port` the port it would be asked
 * at.  Not always the one last told: a lease that names a TFTP server beats
 * anything a card or a keyboard said, since whoever runs the network is the
 * better authority.  This is the one to put on a screen -- showing what was
 * typed instead would show an address nothing is fetching from. */
[[nodiscard]] const uint8_t* fetch_server(uint16_t* port);

/* Where the bytes go, and how far along it is.  Both are defined by whoever
 * links this rather than passed in, so that fetching owns the network and the
 * caller owns the destination: a buffer for a catalogue, the card itself for a
 * disk image, and neither known here.
 *
 * `offset` counts from the start of the file, so a store need keep no position
 * of its own.  A block is 512 bytes -- one sector -- until the last, which is
 * shorter unless the file divides exactly.
 *
 * `total` is what the server said the file is, or 0 where it declined to say.
 * An image is a thousand blocks and more, and a screen with nothing moving on
 * it reads as a machine that has hung. */
void fetch_store(uint32_t offset, const uint8_t* bytes, uint16_t length);
void fetch_progress(uint32_t so_far, uint32_t total);

/* Reads `name`, storing at most `limit` bytes, and says how many arrived.
 *
 * `length` is set on FetchOk and on FetchLost, where it says how far the
 * transfer got -- those bytes were stored, and nothing should be read from
 * them.  A file whose size the server states up front is refused before the
 * first block when it would not fit, which is what tsize is asked for. */
[[nodiscard]] enum FetchResult fetch_file(const char* name, uint32_t limit, uint32_t* length);

/* The code behind FetchRefused, RFC 1350 §5: 1 is no such file, 2 is a file the
 * server will not part with. */
[[nodiscard]] uint8_t fetch_error(void);
