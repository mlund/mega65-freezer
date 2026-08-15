// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* The three exchanges a fetch is made of -- a lease, an address resolved, a
 * transfer -- and the clock all three are measured against. */

#include "fetch.h"

#include "arp.h"
#include "dhcp.h"
#include "dma.h"
#include "eth.h"
#include "ip.h"
#include "netpoll.h"
#include "tftp.h"

#include <mega65.h>
#include <string.h>

/* Far too much for the 16-bit window to spare, so both live in link.ld's
 * .netbuf above the code.  What arrives is whatever a server chose to send: a
 * DHCP offer carries a longer option list than anything this asks for, and a
 * full block plus its headers and check sequence is larger still. */
static __attribute__((section(".netbuf"))) uint8_t net_in[ETH_MAX_RECEIVED];
static __attribute__((section(".netbuf"))) uint8_t net_out[DHCP_FRAME_BYTES];
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
static uint16_t frames_elapsed;

static void tick(void) {
    const uint8_t now = VICIV.fn_raster_msb & RASTER_HIGH;
    if (now < last_high) {
        frames_elapsed++;
    }
    last_high = now;
}

static void restart_clock(void) {
    last_high = VICIV.fn_raster_msb & RASTER_HIGH;
    frames_elapsed = 0;
}

/* How long to wait for an exchange, and how often to say the current step again
 * while it has not moved.  A broadcast from an unconfigured machine is lost in
 * the ordinary course of things, and one attempt means one loss costs the whole
 * fetch. */
static constexpr uint16_t WAIT_FRAMES = 150; /* three seconds */
static constexpr uint16_t RESEND_FRAMES = 25;
/* A disk image is 1600 blocks, one outstanding at a time.  Long enough that a
 * slow server is waited for; short enough that a machine with the wire pulled
 * out gives an answer rather than hanging. */
static constexpr uint16_t TRANSFER_FRAMES = 1800; /* half a minute */

/* Kept between fetches: taking a lease costs seconds, and the address this
 * machine answers for is the same one all through a session. */
static struct DhcpClient lease;
static struct NetEndpoint us;
static uint8_t server_mac[MAC_BYTES];
static uint8_t told_server[IPV4_BYTES];
static uint16_t told_port;
static uint8_t error_code;

void fetch_set_server(const uint8_t* ip, uint16_t port) {
    memcpy(told_server, ip, IPV4_BYTES);
    told_port = port;
    /* A server named after one was resolved is a different machine, so what
     * was resolved is no longer the answer. */
    memset(server_mac, 0, MAC_BYTES);
}

/* Whom to ask, the lease first. */
static const uint8_t* server_address(void) {
    return lease.lease.has_tftp ? lease.lease.tftp : told_server;
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
    restart_clock();
    uint16_t said_at = (uint16_t)(0 - RESEND_FRAMES);
    uint16_t said = 0;
    while (frames_elapsed < WAIT_FRAMES && !dhcp_leased(&lease)) {
        tick();
        if (said) {
            eth_send(net_out, said);
            said_at = frames_elapsed;
        }
        const uint16_t got = net_poll(net_in, sizeof net_in, &us);
        said = got ? dhcp_step(&lease, net_in, got, net_out) : 0;
        if (!said && frames_elapsed - said_at >= RESEND_FRAMES) {
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
    restart_clock();
    uint16_t asked_at = (uint16_t)(0 - RESEND_FRAMES);
    while (frames_elapsed < WAIT_FRAMES && net_zero(server_mac, MAC_BYTES)) {
        tick();
        if (frames_elapsed - asked_at >= RESEND_FRAMES) {
            eth_send(net_out, arp_request(net_out, us.mac, us.ip, hop));
            asked_at = frames_elapsed;
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

enum FetchResult fetch_file(const char* name, Addr28 into, uint32_t limit, uint32_t* length) {
    *length = 0;
    error_code = 0;
    if (!leased()) {
        return FetchNoLease;
    }
    if (net_zero(server_address(), IPV4_BYTES)) {
        return FetchNoServer;
    }
    if (!resolved()) {
        return FetchNoAnswer;
    }

    struct NetEndpoint server = {0};
    memcpy(server.mac, server_mac, MAC_BYTES);
    memcpy(server.ip, server_address(), IPV4_BYTES);
    /* A lease names an address and no port with it, so one it named is asked at
     * the reserved port; a told one is asked wherever it was put. */
    server.port = lease.lease.has_tftp ? 0 : told_port;
    struct TftpClient transfer;
    tftp_start(&transfer, &us, &server, name, VICIV.fn_raster_lsb);

    restart_clock();
    uint16_t said = tftp_step(&transfer, nullptr, 0, net_out);
    uint16_t said_at = 0;
    uint32_t written = 0;
    while (frames_elapsed < TRANSFER_FRAMES && !tftp_done(&transfer) && !tftp_failed(&transfer)) {
        tick();
        if (said) {
            eth_send(net_out, said);
            said_at = frames_elapsed;
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
        if (got && transfer.data_length) {
            if (written + transfer.data_length > limit) {
                return FetchTooBig;
            }
            lcopy((Addr28)(uint16_t)&net_in[TFTP_DATA_AT],
                into + (Addr28)written,
                transfer.data_length);
            written += transfer.data_length;
        }
        if (!said && frames_elapsed - said_at >= RESEND_FRAMES) {
            said = tftp_step(&transfer, nullptr, 0, net_out);
        }
    }
    /* The acknowledgement the last block earned: the loop ends on it rather
     * than sending it, and a server that never hears it records a failure for
     * a file that arrived whole. */
    if (said) {
        eth_send(net_out, said);
    }

    *length = written;
    if (tftp_failed(&transfer)) {
        error_code = (uint8_t)transfer.error;
        return FetchRefused;
    }
    return tftp_done(&transfer) ? FetchOk : FetchLost;
}
