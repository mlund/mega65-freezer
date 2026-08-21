// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* The three exchanges an HTTP fetch is made of -- a lease, an address
 * resolved, a connection -- and the two clocks all three are measured
 * against. */

#include "fetch.h"

#include "arp.h"
#include "dhcp.h"
#include "eth.h"
#include "format.h"
#include "http.h"
#include "ip.h"
#include "netpoll.h"
#include "sdcard.h"

#include <mega65.h>
#include <string.h>

/* Far too much for the 16-bit window to spare, so both live in link.ld's
 * .netbuf above the code.  A DHCP offer carries a longer option list than
 * anything this asks for, and a full segment with its headers is larger
 * still. */
static __attribute__((section(".netbuf"))) uint8_t net_in[ETH_MAX_RECEIVED];
static __attribute__((section(".netbuf"))) uint8_t net_out[DHCP_FRAME_BYTES];
static_assert(sizeof net_out >= HTTP_SEND_BYTES, "no room to build a request");
static_assert(sizeof net_in >= HTTP_RECEIVE_BYTES, "a full segment would be dropped");

/* One storage transaction: two sectors in a single CMD25 stream, which is what
 * makes writing an 800KB image bearable.  Two and not four because .netbuf has
 * no room for more beside the two frame buffers. */
static constexpr uint16_t FETCH_STAGE_BYTES = 2 * SD_SECTOR_SIZE;
static __attribute__((section(".netbuf"))) uint8_t stage[FETCH_STAGE_BYTES];

/* Writes the whole sectors in one stream and pads only a short tail.
 *
 * The two-sector case cannot be folded into the one-sector case:
 * fat32_write_file_sectors() with a count of 1 takes the first-block branch
 * and never closes the stream (fat32.c), which breaks the next write whatever
 * it is.  The split is load-bearing, not clutter. */
static bool store_stage(uint32_t offset, uint16_t bytes) {
    if (bytes == FETCH_STAGE_BYTES) {
        return fetch_store_blocks(offset, stage, FETCH_STAGE_BYTES / SD_SECTOR_SIZE);
    }
    if (bytes > SD_SECTOR_SIZE) {
        return fetch_store(offset, stage, SD_SECTOR_SIZE)
            && fetch_store(offset + SD_SECTOR_SIZE, &stage[SD_SECTOR_SIZE],
                (uint16_t)(bytes - SD_SECTOR_SIZE));
    }
    return fetch_store(offset, stage, bytes);
}

/* Counted in video frames rather than in poll iterations: the poll must run
 * tight, because the controller queues only three frames and three of them
 * arrive in nineteen microseconds, so a loop that sleeps between looks drops
 * what it is waiting for.
 *
 * Only the physical raster's high bits ($D053.0-2), which read 0 below line
 * 256 and 1 above it, so one register says where in the frame we are and a
 * 1 -> 0 step is a frame boundary.  Not $D012, which wraps twice per frame. */
static constexpr uint8_t RASTER_HIGH = 0x07;
static uint8_t last_high;
/* Two clocks, because the two questions are different: how long this exchange
 * has been silent, which gives it up, and how long since we last spoke, which
 * says it again. */
static uint16_t frames_elapsed;
static uint8_t frames_since_said;
/* Frames since the transfer began, which frames_elapsed cannot say: every word
 * from the far end winds that one back to zero.  Counted here rather than by
 * watching frames_elapsed change from the poll loop, which cost a 16-bit load,
 * compare and branch on every iteration of the loop that must run tightest. */
static uint16_t frames_total;
static bool stopped;

struct FetchCounters fetch_counters;

static void tick(void) {
    const uint8_t now = VICIV.fn_raster_msb & RASTER_HIGH;
    if (now < last_high) {
        frames_elapsed++;
        frames_since_said++;
        frames_total++;
        /* The keyboard is read here rather than in the loops that call this:
         * once a frame catches a key a hand can press, and the poll around it
         * runs thousands of times a second with three frames of room. */
        if (!stopped) {
            stopped = fetch_cancelled();
        }
    }
    last_high = now;
}

/* How long to wait for an exchange, and how often to say the current step
 * again while it has not moved. */
static constexpr uint16_t WAIT_FRAMES = 150; /* three seconds */
static constexpr uint8_t RESEND_FRAMES = 25;
/* How long a connection may hear nothing new before it is given up on.
 * Silence and not total time: an 800KB image takes many seconds of a healthy
 * wire, so a clock on the whole of it races a large file that is working. */
static constexpr uint16_t STALL_FRAMES = 400; /* eight seconds */

/* Beginning an exchange sets the speaking clock to its limit, so the first
 * thing it does is speak rather than wait a turn for permission. */
static void begin_exchange(void) {
    last_high = VICIV.fn_raster_msb & RASTER_HIGH;
    frames_elapsed = 0;
    frames_since_said = RESEND_FRAMES;
}

/* Starts the silence clock again, and only that: a word from the far end
 * proves the exchange is alive without being an invitation to answer. */
static void heard_from_far_end(void) {
    frames_elapsed = 0;
}

/* Kept between fetches: taking a lease costs seconds, and the address this
 * machine answers for is the same one all through a session. */
static struct DhcpClient lease;
static struct NetEndpoint us;
static uint8_t server_mac[MAC_BYTES];
static uint8_t told_server[IPV4_BYTES];
/* Zero until somebody names a proxy, and read as HTTP's own port below.
 *
 * Left uninitialised rather than given its default here: a non-zero
 * initialiser is the only thing in the whole tool that wants .zp.data, and it
 * pulls in the start-up copy that section needs.  Measured at 13 bytes for two
 * bytes of value. */
static uint16_t told_port;
static uint16_t status_code;
/* What the Host header carries.  Empty until something names the proxy, and
 * then filled from the address instead. */
static char host_text[HTTP_HOST_MAX + 1];
static_assert(FETCH_SERVER_TEXT_BYTES > HTTP_HOST_MAX,
    "the proxy line must hold a host name and an address before it");

__attribute__((noinline)) void fetch_set_server(const uint8_t* ip, uint16_t port,
    const char* host) {
    memcpy(told_server, ip, IPV4_BYTES);
    /* Resolved here rather than passed on as a zero: three modules holding an
     * opinion about what port 0 means is two too many. */
    told_port = port;
    host_text[0] = 0;
    if (host) {
        uint8_t i = 0;
        for (; i < HTTP_HOST_MAX && host[i]; i++) {
            host_text[i] = host[i];
        }
        host_text[i] = 0;
    }
    /* A server named after one was resolved is a different machine. */
    memset(server_mac, 0, MAC_BYTES);
}

const uint8_t* fetch_server(uint16_t* port) {
    /* As told, so a caller can tell "no port was named" from one that was.
     * Which port a bare address means is this file's business, not a
     * screen's. */
    *port = told_port;
    return told_server;
}

uint16_t fetch_status(void) {
    return status_code;
}

/* An address of our own, from whatever answers on this LAN. */
static bool leased(void) {
    if (dhcp_leased(&lease)) {
        return true;
    }
    eth_init();
    uint8_t mac[MAC_BYTES];
    eth_mac(mac);
    us = (struct NetEndpoint){0};
    memcpy(us.mac, mac, MAC_BYTES);

    /* Where the beam happens to be is the only thing that differs between two
     * runs of the same tool loaded at the same address. */
    dhcp_start(&lease, mac, VICIV.fn_raster_lsb);
    begin_exchange();
    uint16_t said = 0;
    while (frames_elapsed < WAIT_FRAMES && !dhcp_leased(&lease) && !stopped) {
        tick();
        if (said) {
            eth_send(net_out, said);
            frames_since_said = 0;
        }
        const uint16_t got = net_poll(net_in, sizeof net_in, &us);
        said = got ? dhcp_step(&lease, net_in, got, net_out) : 0;
        if (!said && frames_since_said >= RESEND_FRAMES) {
            said = dhcp_step(&lease, nullptr, 0, net_out);
        }
    }
    if (!dhcp_leased(&lease)) {
        return false;
    }
    /* The lease becomes this machine's address here and nowhere else: `us` is
     * what every later exchange means by "who we are". */
    memcpy(us.ip, lease.lease.ip, IPV4_BYTES);
    return true;
}

/* Whose hardware address carries the frames, which is not the address they are
 * sent to when the proxy is off this subnet -- and it always is. */
static bool resolved(void) {
    if (!net_zero(server_mac, MAC_BYTES)) {
        return true;
    }
    const uint8_t* hop =
        ip_next_hop(told_server, us.ip, lease.lease.netmask, lease.lease.router);
    begin_exchange();
    while (frames_elapsed < WAIT_FRAMES && net_zero(server_mac, MAC_BYTES) && !stopped) {
        tick();
        if (frames_since_said >= RESEND_FRAMES) {
            eth_send(net_out, arp_request(net_out, us.mac, us.ip, hop));
            frames_since_said = 0;
        }
        /* An ARP exchange is 42 bytes on the wire and comes back as 64 with
         * its check sequence, so nothing longer need reach the DMA. */
        const uint16_t got = net_poll(net_in, ETH_MIN_RECEIVED, &us);
        if (got) {
            (void)arp_reply_from(net_in, got, hop, server_mac);
        }
    }
    return !net_zero(server_mac, MAC_BYTES);
}

/* The address in the text a Host header carries, for a proxy nobody named. */
static void name_host(void) {
    if (host_text[0]) {
        return; /* named, and a name beats an address to a virtual host */
    }
    char* at = host_text;
    for (uint8_t i = 0; i < IPV4_BYTES; i++) {
        if (i) {
            *at++ = '.';
        }
        at = append_dec(at, told_server[i]);
    }
    /* A port that is not HTTP's belongs in the header, RFC 1945 section 3.2:
     * a proxy behind a virtual host answers on the name it was asked for. */
    if (told_port) {
        *at++ = ':';
        at = append_dec(at, told_port);
    }
    *at = 0;
}

enum FetchResult fetch_file(const char* path, uint32_t limit, uint32_t* length) {
    *length = 0;
    status_code = 0;
    stopped = false;
    fetch_counters = (struct FetchCounters){0};
    if (!leased()) {
        return stopped ? FetchStopped : FetchNoLease;
    }
    if (net_zero(told_server, IPV4_BYTES)) {
        return FetchNoServer;
    }
    if (!resolved()) {
        return stopped ? FetchStopped : FetchNoAnswer;
    }

    struct NetEndpoint server = {0};
    memcpy(server.mac, server_mac, MAC_BYTES);
    memcpy(server.ip, told_server, IPV4_BYTES);
    /* Resolved here, at the one place a connection is actually opened. */
    server.port = told_port ? told_port : HTTP_DEFAULT_PORT;
    name_host();

    struct HttpClient transfer;
    http_start(&transfer, &us, &server, path, host_text, VICIV.fn_raster_lsb);

    begin_exchange();
    frames_total = 0;
    uint16_t said = http_step(&transfer, nullptr, 0, net_out);
    uint32_t written = 0;
    uint32_t stage_offset = 0;
    uint16_t stage_bytes = 0;
    const bool stage_writes = fetch_stores_image();
    while (frames_elapsed < STALL_FRAMES && !http_done(&transfer) && !http_failed(&transfer)
        && !stopped) {
        tick();
        if (said) {
            eth_send(net_out, said);
            frames_since_said = 0;
        }
        /* net_poll() and not eth_receive(): a transfer is exactly when
         * somebody asks who holds this address, and a question left unanswered
         * stops the segments arriving however well the rest of this works. */
        const uint16_t got = net_poll(net_in, HTTP_RECEIVE_BYTES, &us);
        said = got ? http_step(&transfer, net_in, got, net_out) : 0;
        /* Silence is measured against this connection, not against new bytes:
         * a segment the client is right to drop still proves the server is
         * there, and other LAN traffic says nothing about it. */
        if (got && http_heard(&transfer)) {
            fetch_counters.heard++;
            heard_from_far_end();
        }
        /* Read where the step is made: data_length answers for that step, and
         * this loop polls many times between segments. */
        /* Refused before a byte is stored where the server said how big the
         * body is and it would not fit -- which is the whole reason to read
         * Content-Length at all.  Above the store below, or the first
         * segment's bytes are already written down by the time this is
         * noticed. */
        if (got && transfer.has_length && transfer.length > limit) {
            return FetchTooBig;
        }
        if (got && transfer.data_length) {
            if (!written) {
                fetch_counters.frames_to_first_byte = frames_total;
            }
            /* And at the brink, for a server that never said. */
            if (written + transfer.data_length > limit) {
                return FetchTooBig;
            }
            if (stage_writes) {
                /* However the server cut this segment, into whole sectors:
                 * what does not fit the stage starts the next one, which is
                 * the whole of what a byte stream costs over fixed blocks. */
                const uint8_t* at = &net_in[transfer.data_at];
                uint16_t left = transfer.data_length;
                while (left) {
                    if (!stage_bytes) {
                        stage_offset = written;
                    }
                    const uint16_t room = (uint16_t)(FETCH_STAGE_BYTES - stage_bytes);
                    const uint16_t take = left < room ? left : room;
                    lcopy((Addr28)(uint16_t)at, (Addr28)(uint16_t)&stage[stage_bytes], take);
                    stage_bytes = (uint16_t)(stage_bytes + take);
                    at += take;
                    left = (uint16_t)(left - take);
                    written += take;
                    if (stage_bytes == FETCH_STAGE_BYTES) {
                        if (!store_stage(stage_offset, stage_bytes)) {
                            return FetchWriteFailed;
                        }
                        stage_bytes = 0;
                    }
                }
            } else {
                if (!fetch_store(written, &net_in[transfer.data_at], transfer.data_length)) {
                    return FetchWriteFailed;
                }
                written += transfer.data_length;
            }
            /* `length` is zero until a server states one, so it says "not
             * stated" by itself -- which the catalogue endpoint never does. */
            fetch_progress(written, transfer.length, http_done(&transfer), false);
        }
        if (!said && frames_since_said >= RESEND_FRAMES) {
            said = http_step(&transfer, nullptr, 0, net_out);
            fetch_counters.stalls++;
            /* Nothing new has arrived and the step has been said again, so the
             * screen must show that something is still being tried. */
            if (written) {
                fetch_progress(written, transfer.length, false, true);
            }
        }
    }
    /* The acknowledgement the last segment earned: the loop ends on it rather
     * than sending it. */
    if (said) {
        eth_send(net_out, said);
    }

    /* The tail the last full stage left behind, at the same offset and length
     * the loop would have used.  Only on success: a fetch that stopped part
     * way has already been reported, and writing its remainder would put a
     * further stretch of a wrong file on the card. */
    if (http_done(&transfer) && stage_bytes && !store_stage(stage_offset, stage_bytes)) {
        return FetchWriteFailed;
    }

    fetch_counters.bytes = written;
    fetch_counters.dropped = http_dropped(&transfer);
    fetch_counters.retransmits = http_retransmits(&transfer);
    fetch_counters.frames_total = frames_total;
    *length = written;
    status_code = transfer.status;
    if (http_done(&transfer)) {
        return FetchOk;
    }
    if (stopped) {
        return FetchStopped;
    }
    /* A refusal is a status outside 2xx.  Not merely "a status was stated":
     * that is set the moment the status line parses, so a 200 whose connection
     * was reset half way through the body would be reported as the server
     * having said no -- with 200 beside it, which reads as nonsense. */
    if (transfer.status && (transfer.status < 200 || transfer.status >= 300)) {
        return FetchRefused;
    }
    return FetchLost;
}
