// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include "addr28.h"

#include <stdbool.h>
#include <stdint.h>

/* Fetching a named file off the network into memory, in one call.
 *
 * Everything underneath is sans-io and tested on the host; this is where those
 * layers meet the wire, and so the one part of the stack that can only be tried
 * on hardware.  A caller is given an answer and a length rather than a state
 * machine: taking a lease, finding whose hardware address carries the frames,
 * which frame is ours, when to say a thing again and how long to wait each have
 * one sensible answer, and no caller would choose differently.
 *
 * The lease is taken once and kept, so a second fetch is only a transfer. */

/* How it went.  Told apart because they are four different things to do about
 * it: no network, no server, a server that refused, and one that stopped. */
enum FetchResult : uint8_t {
    FetchOk,
    FetchNoLease,  /* nothing answered, so this machine has no address */
    FetchNoServer, /* the lease named no TFTP server, or nobody holds its address */
    FetchRefused,  /* the server said no; fetch_error() is its code */
    FetchLost,     /* it stopped part way, and what was written is incomplete */
    FetchTooBig,   /* more than the caller offered room for, refused before writing */
};

/* Reads `name` into `into`, writing at most `limit` bytes, and says how many
 * arrived.
 *
 * `length` is set on FetchOk and on FetchLost, where it says how far the
 * transfer got -- the bytes are there, but nothing should be read from them.
 * A file whose size the server states up front is refused before the first
 * block when it would not fit, which is what the tsize option is asked for. */
[[nodiscard]] enum FetchResult fetch_file(
    const char* name, Addr28 into, uint32_t limit, uint32_t* length);

/* The code behind FetchRefused, RFC 1350 §5: 1 is no such file, 2 is a file the
 * server will not part with. */
[[nodiscard]] uint8_t fetch_error(void);

/* The address a lease was taken for, all zeros until one is.  Worth showing on
 * a screen: it is the difference between a network that is not there and a
 * server that is not. */
[[nodiscard]] const uint8_t* fetch_address(void);
