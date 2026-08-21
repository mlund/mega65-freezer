// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include "net.h"
#include "tcp.h"

#include <stdbool.h>
#include <stdint.h>

/* Fetching one file over HTTP: the request, the reply's headers, and where its
 * body begins.  TCP stays behind it -- a caller names a path and is handed
 * bytes, never a sequence number or a window -- and a whole frame in, a whole
 * frame out, so this compiles for the host and is tested there.
 *
 * Deliberately not a general client.  The proxy accepts HTTP/1.0, answers
 * `Connection: close` and never chunks -- measured, not assumed -- so there is
 * no chunk decoder, keep-alive or redirect following.  A reply needing any of
 * them fails rather than being half understood. */

/* The longest path that can be asked for, which is the catalogue's own path
 * field (ether65 docs/FILEHOST.md section 2): a client asks for the catalogue
 * endpoint and for the paths the catalogue named, and nothing else exists to
 * ask for.  A longer one is refused rather than truncated -- a truncated path
 * names a different file.  Stated here rather than taken from catalog.h, which
 * this has no business including: a test pins the two together. */
constexpr uint8_t HTTP_PATH_MAX = 48;
/* What goes in the Host header.  An address in text, with room for a port
 * after it, since the proxy is reached by address -- naming it would need a
 * resolver, which is a separate piece of work. */
constexpr uint8_t HTTP_HOST_MAX = 32;
/* Where a request goes when a caller names no port of its own. */
constexpr uint16_t HTTP_DEFAULT_PORT = 80;

/* The request, which is the same shape every time:
 *
 *   GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n
 *
 * `Connection: close` is sent although HTTP/1.0 already means it: the body of
 * the catalogue endpoint has no stated length and so ends at the close, and a
 * proxy someone else is running that kept the connection open would leave that
 * fetch waiting for a close that never comes.  Nineteen bytes to remove a
 * hang. */
constexpr uint16_t HTTP_REQUEST_BYTES =
    (sizeof "GET " - 1) + HTTP_PATH_MAX + (sizeof " HTTP/1.0\r\nHost: " - 1) + HTTP_HOST_MAX
    + (sizeof "\r\nConnection: close\r\n\r\n" - 1);

/* What the send buffer must hold.  The request is the largest thing sent; the
 * SYN's option is smaller and every other segment is a bare acknowledgement. */
constexpr uint16_t HTTP_SEND_BYTES = TCP_PAYLOAD_AT + HTTP_REQUEST_BYTES;
/* And the receive buffer: a whole frame, since the server chooses how much of
 * the body each one carries. */
constexpr uint16_t HTTP_RECEIVE_BYTES = ETH_MAX_RECEIVED;

/* How much of one header line is kept.  Only two are read -- the status line
 * and Content-Length -- and both state what they mean in their first thirty
 * characters, so a longer line keeps its beginning and discards the rest.
 * That is safe because what is matched is a prefix: a truncated line cannot
 * become a different header than the one it started as. */
constexpr uint8_t HTTP_LINE_BYTES = 40;

/* Where the reply has got to.  Status and Headers are apart because a reply
 * that is not a reply at all -- a proxy answering in some other protocol --
 * has to fail on its first line rather than be read as a header block. */
enum HttpStage : uint8_t {
    HttpStatus,
    HttpHeaders,
    HttpBody,
    HttpDone,
    HttpFailed,
};

/* The fetch in progress.
 *
 * `data_at` and `data_length` point into the buffer the last step was given;
 * the body is never copied.  `data_at` is a field because a body has no fixed
 * offset -- the headers are whatever length the server made them.
 *
 * `status` is sixteen bits because three digits do not fit in eight, and is
 * set as soon as the status line is read, so it is worth showing on failure.
 *
 * `has_length` because a file gives Content-Length and the catalogue endpoint
 * does not.  `received` counts body bytes handed over. */
struct HttpClient {
    struct TcpClient tcp;
    uint32_t length;
    uint32_t received;
    uint16_t status;
    uint16_t data_at;
    uint16_t data_length;
    uint8_t request[HTTP_REQUEST_BYTES];
    char line[HTTP_LINE_BYTES];
    uint8_t line_length;
    enum HttpStage stage;
    bool has_length;
};

/* Begins a GET of `path` from `server`, naming `host` in the request.
 *
 * `path` and `host` are read here and not kept -- the request is assembled
 * once into the client, TCP re-reading it on every retransmission -- so a
 * caller may reuse whatever it built them in.
 *
 * `server->port` names the port, 80 being HTTP's.  `seed` moves this
 * connection's port and sequence numbers off the last one's.  Too long a path
 * or host, a zero port, or no address of our own fails with nothing to send. */
void http_start(struct HttpClient* client,
    const struct NetEndpoint* us,
    const struct NetEndpoint* server,
    const char* path,
    const char* host,
    uint16_t seed);

/* Cranks the fetch: returns the length of the frame left in `out`, or 0 when
 * there is nothing to send.  `in` is a received frame; `in_length` 0 means the
 * clock ticked, so say the current step again.
 *
 * The whole loop: start, step with nothing, send what comes back; then step
 * with every frame and on every timeout, sending anything non-zero and taking
 * `data_length` bytes from `data_at` wherever they belong, until http_done()
 * or http_failed(). */
[[nodiscard]] uint16_t http_step(
    struct HttpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out);

/* Whether the last frame belonged to this fetch -- what a caller times silence
 * against, since a segment we are right to drop still proves the server is
 * there. */
[[nodiscard]] static inline bool http_heard(const struct HttpClient* client) {
    return tcp_heard(&client->tcp);
}

#ifdef ETH_COUNTERS
/* How the transport fared underneath, for a caller reporting on the wire
 * rather than on the reply.  Forwarded rather than reached for: `struct
 * TcpClient` is this module's own, and a caller that read its fields could not
 * be told that it never sees a handshake or a window. */
[[nodiscard]] static inline uint16_t http_dropped(const struct HttpClient* client) {
    return client->tcp.dropped;
}

[[nodiscard]] static inline uint16_t http_retransmits(const struct HttpClient* client) {
    return client->tcp.retransmits;
}

#endif

/* Whether the whole body arrived: all of Content-Length where the server
 * stated one, and everything up to the close where it did not. */
[[nodiscard]] static inline bool http_done(const struct HttpClient* client) {
    return client->stage == HttpDone;
}

/* Whether the server refused it, answered something this cannot read, or
 * closed part way through a body whose length it had already stated.  On the
 * first of those `status` says what it refused with. */
[[nodiscard]] static inline bool http_failed(const struct HttpClient* client) {
    return client->stage == HttpFailed;
}
