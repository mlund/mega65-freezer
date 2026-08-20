// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* The three exchanges a fetch is made of -- a lease, an address resolved, a
 * transfer -- and the two clocks all three are measured against. */

#include "fetch.h"

#include "arp.h"
#include "dhcp.h"
#include "dma.h"
#include "eth.h"
#include "ip.h"
#include "netpoll.h"
#include "sdcard.h"
#include "tftp.h"

#include <mega65.h>
#include <string.h>

/* Far too much for the 16-bit window to spare, so both live in link.ld's
 * .netbuf above the code.  What arrives is whatever a server chose to send: a
 * DHCP offer carries a longer option list than anything this asks for, and a
 * full block plus its headers and check sequence is larger still. */
static __attribute__((section(".netbuf"))) uint8_t net_in[ETH_MAX_RECEIVED];
static __attribute__((section(".netbuf"))) uint8_t net_out[DHCP_FRAME_BYTES];
/* One storage transaction in either TFTP mode: two 512-byte blocks or one
 * negotiated 1024-byte block.  Two sectors and not four: a stage must be a
 * whole number of blocks in either mode, and 2048 does not fit .netbuf beside
 * net_in and net_out.  A wider one would amortise the CMD25 stream further. */
static constexpr uint16_t FETCH_STAGE_BYTES = 2 * SD_SECTOR_SIZE;
/* take_options() refuses a granted block that would not tile this, and names
 * the sizes itself rather than being told the stage, which costs less than
 * threading it through the protocol for a rule with one caller.  These keep the
 * two in step: change the stage and the build stops here, rather than a server
 * granting 1024 overrunning the buffer. */
static_assert(FETCH_STAGE_BYTES % TFTP_BLOCK_BYTES == 0, "the default block must tile the stage");
/* store_stage()'s short-tail branch writes one sector and a remainder, so a
 * wider stage needs that branch rewritten, not just the constant changed. */
static_assert(FETCH_STAGE_BYTES <= 2 * SD_SECTOR_SIZE, "a wider stage needs a longer tail path");
static_assert(
    FETCH_STAGE_BYTES % TFTP_MAX_BLOCK_BYTES == 0, "the negotiated block must tile the stage");
static __attribute__((section(".netbuf"))) uint8_t fetch_stage[FETCH_STAGE_BYTES];
static_assert(sizeof net_out >= TFTP_SEND_BYTES, "no room to build a request");
static_assert(sizeof net_in >= TFTP_RECEIVE_BYTES, "a full block would be dropped");

/* Counted in video frames rather than in poll iterations: the poll must run
 * tight, because the controller queues only three frames and three of them
 * arrive in nineteen microseconds, so a loop that sleeps between looks drops
 * what it is waiting for.
 *
 * Only the physical raster's high bits ($D053.0-2), which read 0 below line 256
 * and 1 above it, so one register says where in the frame we are and a 1 -> 0
 * step is a frame boundary.  Not $D012, which wraps twice per frame; and not
 * the two together, which cannot be read at the same instant, so a read
 * straddling line 256 invents a frame.  test/ethtest.c counts frames the same
 * way and keeps its own: it drives the layers under this one directly, and
 * times phases this file has no notion of. */
static constexpr uint8_t RASTER_HIGH = 0x07;
static uint8_t last_high;
/* Two clocks, because the two questions are different: how long this exchange
 * has been silent, which gives it up, and how long since we last spoke, which
 * says it again.  One counter reset for both would tie the answer to the last
 * frame heard -- and a server repeating the block whose acknowledgement went
 * missing would then push back the very answer it is asking for. */
static uint16_t frames_elapsed;
/* A byte is enough: this one is zeroed by every send, and the longest any
 * exchange leaves it running is WAIT_FRAMES past its seed.  Sixteen bits cost
 * more across the increment and the six sites that touch it. */
static uint8_t frames_since_said;

/* Whether this fetch was stopped by hand.  Remembered rather than asked again
 * at the end: the seam reports a keypress and taking it off the queue is what
 * consuming it means, so a second question would answer no. */
static bool stopped;

#ifdef ETH_COUNTERS
/* How far received frames reach once the controller gave them to net_poll().
 * `eth_rx_rotates` in eth.c is every frame the CPU took; these two tell whether
 * it belonged to the TFTP transfer and whether it advanced the file.  Named for
 * this file rather than for the protocol: they count what fetch_file() saw, and
 * test/ethtest.c drives tftp_step() without touching them. */
uint32_t fetch_heard_count;
uint32_t fetch_block_count;
#define COUNT_TFTP(counter) ((counter)++)
#else
#define COUNT_TFTP(counter) ((void)0)
#endif

/* Advances the clocks, once per video frame, and reads the keyboard while it
 * is there. */
static void tick(void) {
    const uint8_t now = VICIV.fn_raster_msb & RASTER_HIGH;
    if (now < last_high) {
        frames_elapsed++;
        frames_since_said++;
        /* The keyboard is read here rather than in the loops that call this:
         * once a frame is often enough to catch a key a hand can press, and
         * the poll around it runs thousands of times a second with only three
         * frames of room in the controller. */
        if (!stopped) {
            stopped = fetch_cancelled();
        }
    }
    last_high = now;
}

/* How long to wait for an exchange, and how often to say the current step again
 * while it has not moved.  A broadcast from an unconfigured machine is lost in
 * the ordinary course of things, and one attempt means one loss costs the whole
 * fetch. */
static constexpr uint16_t WAIT_FRAMES = 150; /* three seconds */
static constexpr uint8_t RESEND_FRAMES = 25;

/* Beginning an exchange sets the speaking clock to its limit, so the first
 * thing it does is speak rather than wait a turn for permission.
 *
 * Sending is left inline at its three sites.  Wrapping it with the clock it
 * resets reads better and costs more: a call is not free on this target, and
 * three sites pay for it three times. */
static void begin_exchange(void) {
    last_high = VICIV.fn_raster_msb & RASTER_HIGH;
    frames_elapsed = 0;
    frames_since_said = RESEND_FRAMES;
}

/* Starts the silence clock again, and only that.
 *
 * A function rather than the bare store it compiles to, so the reason sits
 * with the name: a word from the far end proves the exchange is alive without
 * being an invitation to answer, and answering every repeat is the doubling
 * RFC 1123 §4.2.3.1 warns of. */
static void heard_from_far_end(void) {
    frames_elapsed = 0;
}

/* How long a transfer may hear nothing new before it is given up on.  Silence
 * and not total time: an 800KB image is 1600 blocks and takes fifteen seconds
 * of a healthy wire, so a clock on the whole of it races a large file that is
 * working while never troubling a small one that is not.  Long enough to
 * outlast the gateway's own five one-second retries, after which it has
 * abandoned the transfer and nothing more is coming. */
static constexpr uint16_t STALL_FRAMES = 400; /* eight seconds */

/* Kept between fetches: taking a lease costs seconds, and the address this
 * machine answers for is the same one all through a session. */
static struct DhcpClient lease;
static struct NetEndpoint us;
static uint8_t server_mac[MAC_BYTES];
static uint8_t told_server[IPV4_BYTES];
static uint16_t told_port;
static uint8_t error_code;


/* Out of line although it is short: three callers each inlined the copy setup,
 * which measured 6 bytes more than one body and three calls. */
__attribute__((noinline)) void fetch_set_server(const uint8_t* ip, uint16_t port) {
    memcpy(told_server, ip, IPV4_BYTES);
    /* Resolved here rather than passed on as a zero: three modules holding an
     * opinion about what port 0 means is two too many. */
    told_port = port ? port : TFTP_PORT;
    /* A server named after one was resolved is a different machine, so what
     * was resolved is no longer the answer. */
    memset(server_mac, 0, MAC_BYTES);
}

/* Whom to ask, the lease first.  A lease names an address and no port with it,
 * so one it named is asked at TFTP's own. */
static const uint8_t* server_address(void) {
    return lease.lease.has_tftp ? lease.lease.tftp : told_server;
}

const uint8_t* fetch_server(uint16_t* port) {
    *port = lease.lease.has_tftp ? TFTP_PORT : told_port;
    return server_address();
}

uint8_t fetch_error(void) {
    return error_code;
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
     * what every later exchange means by "who we are", and an address left in
     * the lease alone is one udp_parse() would never match against. */
    memcpy(us.ip, lease.lease.ip, IPV4_BYTES);
    return true;
}

/* Whose hardware address carries the frames, which is not the address they are
 * sent to when the server is off this subnet. */
static bool resolved(void) {
    if (!net_zero(server_mac, MAC_BYTES)) {
        return true;
    }
    const uint8_t* hop =
        ip_next_hop(server_address(), us.ip, lease.lease.netmask, lease.lease.router);
    begin_exchange();
    while (frames_elapsed < WAIT_FRAMES && net_zero(server_mac, MAC_BYTES) && !stopped) {
        tick();
        if (frames_since_said >= RESEND_FRAMES) {
            eth_send(net_out, arp_request(net_out, us.mac, us.ip, hop));
            frames_since_said = 0;
        }
        /* An ARP exchange is 42 bytes on the wire and comes back as 64 with its
         * check sequence, so nothing longer need reach the DMA. */
        const uint16_t got = net_poll(net_in, ETH_MIN_RECEIVED, &us);
        if (got) {
            (void)arp_reply_from(net_in, got, hop, server_mac);
        }
    }
    return !net_zero(server_mac, MAC_BYTES);
}

/* Writes the whole sectors first and pads only the short tail.
 *
 * The two-sector case cannot be folded into the one-sector case:
 * fat32_write_file_sectors() with a count of 1 takes the first-block branch and
 * never closes the CMD25 stream (fat32.c), which breaks the next write whatever
 * it is.  The split is load-bearing, not clutter.
 *
 * One entry point down in fat32.c taking a length, so this file need not know
 * that a run of sectors and a padded tail are written differently, reads better
 * and costs more than the split does -- and more still unless it quietly caps
 * at two sectors. */
static bool store_stage(uint32_t offset, uint16_t bytes) {
    if (bytes == FETCH_STAGE_BYTES) {
        return fetch_store_blocks(offset, fetch_stage, FETCH_STAGE_BYTES / SD_SECTOR_SIZE);
    }
    if (bytes > SD_SECTOR_SIZE) {
        return fetch_store(offset, fetch_stage, SD_SECTOR_SIZE)
            && fetch_store(offset + SD_SECTOR_SIZE, &fetch_stage[SD_SECTOR_SIZE],
                bytes - SD_SECTOR_SIZE);
    }
    return fetch_store(offset, fetch_stage, bytes);
}

enum FetchResult fetch_file(const char* name, uint32_t limit, uint32_t* length) {
    *length = 0;
    error_code = 0;
    stopped = false;
    if (!leased()) {
        return stopped ? FetchStopped : FetchNoLease;
    }
    if (net_zero(server_address(), IPV4_BYTES)) {
        return FetchNoServer;
    }
    if (!resolved()) {
        return stopped ? FetchStopped : FetchNoAnswer;
    }

    struct NetEndpoint server = {0};
    memcpy(server.mac, server_mac, MAC_BYTES);
    memcpy(server.ip, fetch_server(&server.port), IPV4_BYTES);
    struct TftpClient transfer;
    tftp_start(&transfer, &us, &server, name, VICIV.fn_raster_lsb);

    begin_exchange();
    uint16_t said = tftp_step(&transfer, nullptr, 0, net_out);
    uint32_t written = 0;
    uint32_t stage_offset = 0;
    uint16_t stage_bytes = 0;
    const bool stage_writes = fetch_stores_image();
    while (frames_elapsed < STALL_FRAMES && !tftp_done(&transfer) && !tftp_failed(&transfer)
        && !stopped) {
        tick();
        if (said) {
            eth_send(net_out, said);
            frames_since_said = 0;
        }
        /* net_poll() and not eth_receive(): a transfer is exactly when the
         * server asks who holds this address, and a question left unanswered
         * stops the blocks arriving however well the rest of this works. */
        const uint16_t got = net_poll(net_in, TFTP_RECEIVE_BYTES, &us);
        said = got ? tftp_step(&transfer, net_in, got, net_out) : 0;
        /* Read where the step is made: data_length answers for that step, and
         * this loop polls many times between blocks. */
        /* Refused before a byte is written where the server said how big the
         * file is, and at the brink where it did not. */
        if (got && transfer.has_size && transfer.size > limit) {
            return FetchTooBig;
        }
        /* Silence is measured against this transfer, not against new bytes: its
         * server may repeat the last block while an acknowledgement is lost,
         * and answering that repeat directly is the RFC 1123 doubling trap.  A
         * TFTP word that belongs here is proof the transfer is alive; other LAN
         * traffic says nothing about it. */
        if (got && tftp_heard(&transfer)) {
            COUNT_TFTP(fetch_heard_count);
            heard_from_far_end();
        }
        if (got && transfer.data_length) {
            COUNT_TFTP(fetch_block_count);
            if (written + transfer.data_length > limit) {
                return FetchTooBig;
            }
            if (stage_writes) {
                if (!stage_bytes) {
                    stage_offset = written;
                }
                lcopy((Addr28)(uint16_t)&net_in[TFTP_DATA_AT],
                    (Addr28)(uint16_t)&fetch_stage[stage_bytes], transfer.data_length);
                stage_bytes += transfer.data_length;
            } else if (!fetch_store(written, &net_in[TFTP_DATA_AT], transfer.data_length)) {
                return FetchWriteFailed;
            }
            written += transfer.data_length;
            /* `size` is zero until a server states one, so it says "not
             * stated" by itself; testing has_size as well measured 23 bytes
             * for an answer that cannot differ.  Read per block rather than
             * held, which measured 53 bytes more. */
            /* Read once: `transfer` escapes to tftp_step(), so the compiler
             * must reload it across the store above and re-emit the compare.
             * tftp_done() is the same conclusion take() drew from the same
             * comparison, and a byte test rather than two 16-bit ones. */
            fetch_progress(written, transfer.size, tftp_done(&transfer), false);
        }
        /* The server may send the next DATA while these sectors wait for the
         * card.  Sending the ACK here, rather than at the next poll, makes the
         * three slots a pipeline instead of merely deferred work.  An SD
         * failure can therefore follow an ACK; FetchWriteFailed preserves the
         * incomplete file rather than claiming the transfer succeeded. */
        if (said && stage_bytes) {
            eth_send(net_out, said);
            frames_since_said = 0;
            said = 0;
        }
        /* Only a full stage: a short block ends the transfer, and the flush
         * after the loop stores what it left behind, at the same offset and
         * length this would have used. */
        if (stage_bytes == FETCH_STAGE_BYTES) {
            if (!store_stage(stage_offset, stage_bytes)) {
                return FetchWriteFailed;
            }
            stage_bytes = 0;
        }
        if (!said && frames_since_said >= RESEND_FRAMES) {
            said = tftp_step(&transfer, nullptr, 0, net_out);
            /* Nothing new has arrived and the request has been said again, so
             * the screen must show that something is still being tried.  Not
             * while the first request is unanswered, where there is no count
             * to repeat. */
            if (written) {
                fetch_progress(written, transfer.size, false, true);
            }
        }
    }
    /* The acknowledgement the last block earned: the loop ends on it rather
     * than sending it, and a server that never hears it records a failure for
     * a file that arrived whole. */
    if (said) {
        eth_send(net_out, said);
    }

    /* TFTP ends an exact multiple with an empty DATA.  The full tail then has
     * no block to force it out, so close it only after the transfer succeeded. */
    if (tftp_done(&transfer) && stage_bytes) {
        if (!store_stage(stage_offset, stage_bytes)) {
            return FetchWriteFailed;
        }
    }

    *length = written;
    if (tftp_failed(&transfer)) {
        error_code = (uint8_t)transfer.error;
        return FetchRefused;
    }
    if (tftp_done(&transfer)) {
        return FetchOk;
    }
    /* Asked for before it was lost: a transfer someone stopped is not a fault
     * to report, and what it left behind is cleared up the same way either. */
    return stopped ? FetchStopped : FetchLost;
}
