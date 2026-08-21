// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* The request HTTP/1.0 fixes, the two reply headers worth reading, and where
 * the body begins and ends. */

#include "http.h"

#include "format.h"

#include <string.h>

/* The status line's own prefix, and the one header that says how long the body
 * is.  Both are matched in lower case: RFC 1945 section 4.2 makes header names
 * case-insensitive, and letters differ only in bit 5. */
static const char HTTP_VERSION[] = "http/1.";
static const char CONTENT_LENGTH[] = "content-length:";

/* Whether `line` begins with `want`, which is given in lower case.  The
 * terminator stops it, so nothing is read past the line. */
[[nodiscard]] static bool prefix_is(const char* line, const char* want) {
    for (; *want; want++, line++) {
        if ((*line | 0x20) != *want) {
            return false;
        }
    }
    return true;
}

/* The status line: the version, then exactly three digits.
 *
 * Checked rather than assumed because the first line is the only place a reply
 * that is not a reply can be caught -- a proxy answering in some other
 * protocol, or an error page from something in between.  Read as a header
 * block instead, it would be a fetch that succeeds and writes down whatever
 * arrived. */
static void take_status(struct HttpClient* client) {
    const char* at = client->line;
    if (!prefix_is(at, HTTP_VERSION)) {
        client->stage = HttpFailed;
        return;
    }
    at += sizeof HTTP_VERSION - 1;
    /* The minor version, then the space before the code. */
    if (*at < '0' || *at > '9' || at[1] != ' ') {
        client->stage = HttpFailed;
        return;
    }
    at += 2;
    uint16_t status = 0;
    for (uint8_t i = 0; i < 3; i++) {
        if (at[i] < '0' || at[i] > '9') {
            client->stage = HttpFailed;
            return;
        }
        status = (uint16_t)(status * 10 + (uint8_t)(at[i] - '0'));
    }
    /* Four digits is not a status code either. */
    if (at[3] >= '0' && at[3] <= '9') {
        client->stage = HttpFailed;
        return;
    }
    /* Kept even on a refusal, and particularly then: 404 and 503 are different
     * things for a caller to say. */
    client->status = status;
    /* 2xx is the only family this can use.  A redirect is not followed --
     * following one needs a second connection and a limit on how many, for a
     * proxy that was measured never to send one. */
    client->stage = (status >= 200 && status < 300) ? HttpHeaders : HttpFailed;
}

/* The one header worth reading.  A value that is not a number leaves the
 * length unstated, which is a body that ends at the close -- the same as a
 * server that never mentioned it. */
static void take_header(struct HttpClient* client) {
    if (!prefix_is(client->line, CONTENT_LENGTH)) {
        return;
    }
    const char* at = &client->line[sizeof CONTENT_LENGTH - 1];
    while (*at == ' ') {
        at++;
    }
    if (*at < '0' || *at > '9') {
        return;
    }
    client->length = parse_dec(at);
    client->has_length = true;
}

/* One complete line of the reply's head. */
static void take_line(struct HttpClient* client) {
    if (client->stage == HttpStatus) {
        take_status(client);
    } else if (client->line_length) {
        take_header(client);
    } else {
        client->stage = HttpBody; /* the blank line, and the head is over */
    }
}

/* The bytes TCP delivered: whatever is still head, a line at a time, and
 * whatever is left after it as body.
 *
 * The head may end anywhere in a segment, which is why the body's offset is
 * worked out here rather than being a constant.  A reply of any size is more
 * than one segment, so the head being cut across two of them is the ordinary
 * case and not an edge. */
static void take(struct HttpClient* client, const uint8_t* in) {
    uint16_t at = client->tcp.data_at;
    uint16_t left = client->tcp.data_length;

    while (left && client->stage < HttpBody) {
        const uint8_t byte = in[at++];
        left--;
        if (byte == '\r') {
            continue; /* the LF behind it is what ends a line */
        }
        if (byte != '\n') {
            /* A line longer than the buffer keeps its beginning and drops the
             * rest.  Safe because what is matched is a prefix: a truncated
             * line cannot become a different header than it started as. */
            if (client->line_length < HTTP_LINE_BYTES - 1) {
                client->line[client->line_length++] = (char)byte;
            }
            continue;
        }
        client->line[client->line_length] = 0;
        take_line(client);
        client->line_length = 0;
    }
    if (client->stage != HttpBody) {
        return;
    }

    uint16_t room = left;
    if (client->has_length) {
        /* A server sending more than it promised does not get the extra into
         * the file: the length is what the caller found room for. */
        const uint32_t remaining = client->length - client->received;
        if (room > remaining) {
            room = (uint16_t)remaining;
        }
    }
    if (room) {
        client->data_at = at;
        client->data_length = room;
        client->received += room;
    }
    /* Over on the last byte of a stated length, without waiting for a close a
     * caller would otherwise have to time out against. */
    if (client->has_length && client->received >= client->length) {
        client->stage = HttpDone;
    }
}

void http_start(struct HttpClient* client,
    const struct NetEndpoint* us,
    const struct NetEndpoint* server,
    const char* path,
    const char* host,
    uint16_t seed) {
    *client = (struct HttpClient){0};
    /* Refused rather than truncated: a truncated path names a different file,
     * and fetching the wrong one quietly is worse than fetching none.  Both
     * are NUL-terminated by construction -- a catalogue record's path, and
     * fetch.c's own host text. */
    if (strlen(path) > HTTP_PATH_MAX || strlen(host) > HTTP_HOST_MAX) {
        client->stage = HttpFailed;
        return;
    }

    /* Assembled once, into the client itself, because TCP re-reads the request
     * on every retransmission and so needs bytes that stay put.  The caller is
     * then free to reuse whatever it built `path` and `host` in. */
    char* at = append_str((char*)client->request, "GET ");
    at = append_str(at, path);
    at = append_str(at, " HTTP/1.0\r\nHost: ");
    at = append_str(at, host);
    at = append_str(at, "\r\nConnection: close\r\n\r\n");

    tcp_start(&client->tcp, us, server, client->request,
        (uint16_t)(at - (char*)client->request), seed);
    if (tcp_failed(&client->tcp)) {
        client->stage = HttpFailed;
    }
}

uint16_t http_step(struct HttpClient* client, const uint8_t* in, uint16_t in_length, uint8_t* out) {
    client->data_length = 0;
    if (client->stage >= HttpDone) {
        return 0;
    }

    const uint16_t said = tcp_step(&client->tcp, in, in_length, out);
    if (client->tcp.data_length) {
        take(client, in);
    }

    if (tcp_failed(&client->tcp)) {
        client->stage = HttpFailed;
    } else if (tcp_done(&client->tcp) && client->stage < HttpDone) {
        /* The connection closed.  That ends a body whose length was never
         * stated -- the catalogue endpoint's, which is why this matters -- and
         * truncates one whose length was. */
        client->stage = (client->stage == HttpBody && !client->has_length) ? HttpDone : HttpFailed;
    }
    return said;
}
