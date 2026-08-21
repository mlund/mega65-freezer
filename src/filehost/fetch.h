// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Fetching a file over HTTP, in one call.
 *
 * Everything underneath is sans-io and tested on the host; this is where those
 * layers meet the wire, and so the one part of the stack that can only be
 * tried on hardware -- xemu has no ethernet.  A caller is given an answer and
 * a length rather than a state machine: taking a lease, finding whose hardware
 * address carries the frames, which frame is ours, when to say a thing again
 * and how long to wait each have one sensible answer.
 *
 * The lease is taken once and kept, so a second fetch is only a connection. */

/* How it went.  Told apart because each is a different thing to do about it. */
enum FetchResult : uint8_t {
    FetchOk,
    FetchNoLease,  /* nothing answered, so this machine has no address */
    FetchNoServer, /* no proxy address: none was told */
    FetchNoAnswer, /* an address, but nobody on the wire holds it */
    FetchRefused,  /* the server said no; fetch_status() is what it said */
    FetchLost,     /* it stopped part way, and what was written is incomplete */
    FetchTooBig,   /* more than the caller offered room for */
    FetchWriteFailed, /* the destination would not take the bytes */
    FetchStopped,  /* the user asked for it to stop; nothing is wrong */
};

/* The longest proxy line there is: an address, a port after a colon, a space,
 * and the name to ask by.  Stated here rather than taken from the protocol's
 * own header, so that a caller sizing a text field does not have to know which
 * protocol is underneath. */
constexpr uint8_t FETCH_SERVER_TEXT_BYTES = 56;

/* The proxy to fetch from: the address to connect to, and the name to ask it
 * by.  Separate things, which cost a 404 to learn -- the public proxy is
 * name-based virtual hosting, so a Host header carrying the address is
 * answered by the wrong site, with a healthy connection and no file.  No
 * resolver is needed; the address still comes from the card.
 *
 * `host` null uses the address, right for a proxy answering whatever it is
 * asked.  `port` 0 means the protocol's own. */
void fetch_set_server(const uint8_t* ip, uint16_t port, const char* host);

/* The proxy a fetch would go to, and through `port` the port it was named
 * with -- 0 where it was named with none, so a caller showing it can leave out
 * what a bare address already says. */
[[nodiscard]] const uint8_t* fetch_server(uint16_t* port);

/* Where the bytes go, and how far along it is.  Defined by whoever links this
 * rather than passed in, so that fetching owns the network and the caller owns
 * the destination.
 *
 * `offset` counts from the start of the body, so a store need keep no position
 * of its own.  The lengths are whatever the server cut its segments at: a body
 * is a byte stream and not a run of fixed blocks, which is the one thing a
 * caller staging whole sectors has to allow for.
 *
 * False where the bytes did not reach their destination.  A fetch stops there:
 * carrying on writes a file that is wrong. */
[[nodiscard]] bool fetch_store(uint32_t offset, const uint8_t* bytes, uint16_t length);

/* Whether this fetch writes its bytes to a contiguous disk image, and the
 * batched write that goes with it: a run of whole sectors in one CMD25 stream
 * rather than one transaction each.
 *
 * The staging behind it has to cope with arrivals of any length: a body is cut
 * wherever the server's buffer ran out, so what arrives straddles the stage
 * and the remainder starts the next one. */
[[nodiscard]] bool fetch_stores_image(void);
[[nodiscard]] bool fetch_store_blocks(uint32_t offset, const uint8_t* bytes, uint8_t count);

/* `total` is Content-Length, or 0 where the server did not state one -- which
 * the catalogue endpoint does not, so a progress bar has to cope with not
 * knowing.  `waiting` says the count has not moved and the step was said
 * again, which is the difference between a fetch that is slow and one that is
 * over. */
void fetch_progress(uint32_t so_far, uint32_t total, bool last, bool waiting);

/* Whether the user has asked for this to stop, asked once per poll of every
 * wait here -- the lease and the address as well as the transfer, since a
 * network that answers nothing is exactly when someone wants out. */
[[nodiscard]] bool fetch_cancelled(void);

/* Fetches `path` from the proxy, storing at most `limit` bytes, and says how
 * many arrived.  `path` is a URL path and begins with a slash.
 *
 * `length` is set on FetchOk and on FetchLost, where it says how far it got.
 *
 * A fetch is given up when nothing new has arrived for a while, rather than
 * when it has run long: an 800KB image takes seconds of a healthy wire, and a
 * clock on the whole of it would end a transfer that was working. */
[[nodiscard]] enum FetchResult fetch_file(const char* path, uint32_t limit, uint32_t* length);

/* The HTTP status behind FetchRefused: 404 is no such file, 403 one the server
 * will not part with.  Sixteen bits because three digits do not fit in
 * eight. */
[[nodiscard]] uint16_t fetch_status(void);

#ifdef ETH_COUNTERS
/* What the transport had to cope with, for the benchmark that decides whether
 * receiving strictly in order is good enough.
 *
 * `dropped` is segments thrown away for falling outside the window -- near
 * zero on a real connection means the reassembly buffer never needs writing.
 * `retransmits` is steps where the clock made us say the same thing again, and
 * `stalls` is waits that ended with nothing new having arrived. */
struct FetchCounters {
    uint16_t dropped;
    uint16_t retransmits;
    uint16_t stalls;
    /* Frames that belonged to this connection.  The receiver is promiscuous,
     * so most of what arrives is somebody else's: against eth.c's own count of
     * every frame taken, this says how much of the wire was ours. */
    uint16_t heard;
    /* What came off the wire, which is not what was kept: a catalogue is 292KB
     * of JSON and 33KB of records.  Here because a rate needs both a time and
     * a length, and this is the only place that has the length. */
    uint32_t bytes;
    /* Video frames from the first request to the last byte, and to the first
     * body byte: the two together separate a slow server from a slow wire. */
    uint16_t frames_total;
    uint16_t frames_to_first_byte;
};

/* A global rather than something behind an accessor, and deliberately: the
 * only reader is test/verify_filehost_hw.py, which looks the symbol up in the
 * ELF because what it wants is the layout.  eth.c's counters are public for
 * the same reason.  A file-static will not do: nothing in the program reads
 * it, so LTO is right to delete it and the hardware test then finds no
 * symbol. */
extern struct FetchCounters fetch_counters;
#endif
