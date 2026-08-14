/*
 * What the ethernet controller actually delivers, dumped to a sector the host
 * can read back.
 *
 * There is no emulator route to the wire on macOS and no serial cable on this
 * machine, so the only way to see a received frame is to have the machine
 * write one down.  This runs standalone -- etherload loads and runs it with no
 * keypresses -- and leaves its findings in the first sector of ETHLOG.DAT,
 * which mega65_ftp's secdump brings back.  A file the test owns, so a wrong
 * sector number cannot damage anything else on the card.
 *
 * Standalone rather than inside the freezer deliberately: the frozen state has
 * already been shown to transmit and receive, so what is left to explain is
 * the buffer handling, which owes nothing to being frozen.
 *
 * The sector is the card's, not the program's, so it has to be found once per
 * card and set in LOG_SECTOR below:
 *
 *   dd if=/dev/zero of=ETHLOG.DAT bs=512 count=1
 *   mega65_ftp -e -y -c "put ETHLOG.DAT" -c "clusters ETHLOG.DAT" -c "secinfo"
 *     sector = <cluster #2's sector> + (cluster - 2) * <sectors per cluster>
 *
 * Then, as often as wanted, with no keypresses on the machine:
 *
 *   etherload -r ethtest.prg
 *   ...wait: the run listens, leases and then answers ARP for a while...
 *   mega65_ftp -e -y -c "secdump out.bin <sector> 1" -c "exit"
 *
 * The sector reads BUSY until the run finishes and ETH2 once it has, so a
 * readback taken too early is obvious rather than plausible.  The border goes
 * green at the end.
 *
 * The addresses below are a LAN's, too: SENDER has to be free on it and TARGET
 * has to be something that answers.
 */

#include "arp.h"
#include "common.h"
#include "dhcp.h"
#include "dma.h"
#include "eth.h"
#include "ip.h"
#include "mega65_regs.h"
#include "sdcard.h"

#include <mega65.h>
#include <string.h>

/* The first sector of ETHLOG.DAT, worked out from its cluster and the card's
 * geometry, and checked by writing a pattern through the filesystem and
 * reading this sector back. */
constexpr uint32_t LOG_SECTOR = 22532;

constexpr uint8_t FRAME_SLICE = 64; /* enough for the headers of anything */
/* Three, not four: the fourth slot would start at 256, which does not fit
 * the uint8_t the offset is held in and wraps onto the header. */
constexpr uint8_t FRAMES_LOGGED = 3;
constexpr uint8_t LOG_HEADER = 64;
/* Where each thing goes in the header.  Named because the header is exactly
 * full: the last lease field ends at 63 and LOG_HEADER is 64, so one more
 * value would silently land on the first frame slice. */
constexpr uint8_t LOG_FRAMES = 4;
constexpr uint8_t LOG_LOGGED = 6;
constexpr uint8_t LOG_MAC = 8;
constexpr uint8_t LOG_CTRL = 16;
constexpr uint8_t LOG_ANSWERED = 26;
constexpr uint8_t LOG_REPLY_MAC = 32;
constexpr uint8_t LOG_SENT = 20;
constexpr uint8_t LOG_DHCP_STAGE = 40;
constexpr uint8_t LOG_DHCP_OFFERS = 41;
constexpr uint8_t LOG_DHCP_ACKS = 43;
constexpr uint8_t LOG_HAS_TFTP = 45;
constexpr uint8_t LOG_ARP_SERVED = 46;
constexpr uint8_t LOG_LEASE = 48;
constexpr uint8_t LOG_LEASE_FIELDS = 4;
static_assert(LOG_LEASE + LOG_LEASE_FIELDS * IPV4_BYTES <= LOG_HEADER,
    "the log header has run into the first frame slice");

/* Long enough to see ordinary LAN traffic, short enough that a run finishes
 * while the host is still waiting for it.  Counted in video frames rather than
 * in poll iterations: the poll has to run tight, because the controller holds
 * only three queued frames and three of them arrive in 19 microseconds, so a
 * loop that sleeps between looks drops what it is there to count. */
constexpr uint16_t LISTEN_FRAMES = 100; /* about two seconds */
/* How often to say it again while the exchange has not moved on.  A broadcast
 * from an unconfigured host is lost in the ordinary course of things, and one
 * attempt per run means a single loss costs the whole run. */
constexpr uint16_t RESEND_FRAMES = 25;
/* Long enough for a person to ping the leased address from the other end and
 * see whether it answers. */
constexpr uint16_t SERVE_FRAMES = 600;

/* Far too much for the 16-bit window.  src/link.ld puts .netbuf above the code
 * at $A200 for the tools; this PRG links the platform's own script and its
 * copy of the section lands wherever lld puts it, which is fine here -- a test
 * PRG has no $9000 ceiling to respect.  Two buffers, because a reply is parsed
 * while the request is still needed.
 *
 * What is sent is a DHCP datagram and its headers; what arrives is whatever
 * the server chose to send, and a server's option list is longer than a
 * client's -- sized at 342 the offers came back truncated, and a truncated
 * datagram is refused rather than half-read. */
static __attribute__((section(".netbuf"))) uint8_t net_out[UDP_PAYLOAD_AT + DHCP_PAYLOAD_BYTES];
static __attribute__((section(".netbuf"))) uint8_t net_in[ETH_MAX_FRAME];

/* Video frames since the last reset.
 *
 * Only the physical raster's high bits ($D053.0-2, iomap.txt:220), which count
 * 0 for lines 0-255 and 1 above that, so one register read says where in the
 * frame we are and a 1 -> 0 step is a frame boundary.  Not $D012: that is
 * eight bits of a 312-line frame, so it wraps twice per frame and a clock
 * counting its wraps runs at double speed.  Not the two registers together
 * either: they cannot be read at the same instant, and a read that straddles
 * line 256 sees the old high bits with the new low ones and invents a frame.
 *
 * The cost of one register is that the high window is only ~3.6ms wide, so a
 * loop must look at least that often.  These poll in microseconds. */
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

static void log_put16(uint8_t at, uint16_t value) {
    sector_buffer[at] = (uint8_t)value;
    sector_buffer[at + 1] = (uint8_t)(value >> 8);
}

int main(void) {
    mega65_fast();
    __asm__ volatile("sei" ::: "memory");

    POKE(0x00, CPU_PORT_DDR_ALL_OUTPUTS);
    POKE(0x01, CPU_PORT_KERNAL_AND_IO);
    /* Without this sdcard.c addresses the card in bytes rather than sectors
     * and the controller never reports the transfer complete. */
    sdhc_card = (SD_STATUS & SD_STATUS_SDHC) != 0;
    /* And this selects the card's sector buffer rather than the floppy's, as
     * src/freezer/main.c does: writes go through $FFD6E00 either way, so with
     * the wrong one selected a write ships whatever the SD buffer last held. */
    SD_MISC = SD_MISC | SD_MISC_BUFSEL_SDCARD;

    for (uint16_t i = 0; i < SD_SECTOR_SIZE; i++) {
        sector_buffer[i] = 0;
    }
    /* Claim the sector before anything else, so a readback taken too early
     * says so.  The run now lasts tens of seconds, and the alternative is a
     * host reading the previous run's sector -- valid magic, plausible lease,
     * entirely stale -- and believing it. */
    sector_buffer[0] = 'B';
    sector_buffer[1] = 'U';
    sector_buffer[2] = 'S';
    sector_buffer[3] = 'Y';
    sdcard_writesector(LOG_SECTOR, 0);

    sector_buffer[0] = 'E';
    sector_buffer[1] = 'T';
    sector_buffer[2] = 'H';
    sector_buffer[3] = '2';

    eth_init();
    uint8_t mac[MAC_BYTES];
    eth_mac(mac);
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        sector_buffer[LOG_MAC + i] = mac[i];
    }
    sector_buffer[LOG_CTRL + 0] = ETHERNET.ctrl1;
    sector_buffer[LOG_CTRL + 1] = ETHERNET.ctrl2;
    sector_buffer[LOG_CTRL + 2] = ETHERNET.ctrl3;

    /* A quarter of the way in, one ARP request and one UDP broadcast go out,
     * and the rest of the run listens.  Per RX event the log takes the decoded
     * length, the controller's flag byte, and the frame as eth_receive()
     * delivered it -- our own frames included, which the source address in the
     * dump tells apart. */
    static constexpr uint8_t SENDER[IPV4_BYTES] = {192, 168, 68, 234};
    static constexpr uint8_t TARGET[IPV4_BYTES] = {192, 168, 68, 57};
    uint16_t frames = 0;
    uint16_t logged = 0;
    bool answered = false;
    uint8_t reply_mac[MAC_BYTES] = {0};

    const uint16_t asked = arp_request(net_out, mac, SENDER, TARGET);
    eth_send(net_out, asked);
    sector_buffer[LOG_SENT] = 1;

    restart_clock();
    while (frames_elapsed < LISTEN_FRAMES && !(answered && logged == FRAMES_LOGGED)) {
        tick();
        uint8_t rx_flags = 0;
        const uint16_t got = eth_receive(net_in, sizeof net_in, &rx_flags);
        if (!got) {
            continue;
        }
        frames++;
        if (!answered && arp_reply_from(net_in, got, TARGET, reply_mac)) {
            answered = true;
        }

        if (logged < FRAMES_LOGGED) {
            const uint8_t at = (uint8_t)(LOG_HEADER + logged * FRAME_SLICE);
            log_put16(at, got);
            sector_buffer[at + 2] = rx_flags;
            for (uint8_t i = 0; i < FRAME_SLICE - 4; i++) {
                sector_buffer[at + 4 + i] = net_in[i];
            }
            logged++;
        }
    }

    /* And now a lease, from whatever answers on this LAN: the exchange the
     * machine has to complete before it has an address of its own. */
    struct NetEndpoint us = {.port = DHCP_CLIENT_PORT};
    struct NetEndpoint server = {.port = DHCP_SERVER_PORT};
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        us.mac[i] = mac[i];
        server.mac[i] = 0xFF;
    }
    for (uint8_t i = 0; i < IPV4_BYTES; i++) {
        server.ip[i] = 255;
    }
    /* Distinct per machine, so two of them asking at once are told apart. */
    const uint32_t xid = 0x4D360000u | ((uint32_t)mac[4] << 8) | mac[5];

    struct DhcpLease lease = {0};
    uint8_t stage = 0; /* 0 discovering, 1 requesting, 2 leased */
    uint16_t offers = 0;
    uint16_t acks = 0;

    uint16_t length = dhcp_discover(&net_out[UDP_PAYLOAD_AT], mac, xid);
    length = udp_build(net_out, &us, &server, &net_out[UDP_PAYLOAD_AT], length);
    eth_send(net_out, length);

    restart_clock();
    uint16_t said_at = 0;
    while (frames_elapsed < LISTEN_FRAMES && stage < 2) {
        tick();
        /* Still waiting on the same step: ask again rather than spend the whole
         * budget on one lost broadcast. */
        if (frames_elapsed - said_at >= RESEND_FRAMES) {
            said_at = frames_elapsed;
            length = (stage == 0) ? dhcp_discover(&net_out[UDP_PAYLOAD_AT], mac, xid)
                                  : dhcp_request(&net_out[UDP_PAYLOAD_AT], mac, xid, &lease);
            length = udp_build(net_out, &us, &server, &net_out[UDP_PAYLOAD_AT], length);
            eth_send(net_out, length);
        }

        uint8_t rx = 0;
        const uint16_t got = eth_receive(net_in, sizeof net_in, &rx);
        if (!got) {
            continue;
        }
        struct UdpDatagram datagram;
        if (!udp_parse(net_in, got, &us, &datagram)) {
            continue;
        }
        const enum DhcpMessage message = dhcp_parse(
            &net_in[UDP_PAYLOAD_AT], datagram.payload_length, xid, mac, &lease);
        if (message == DhcpOffer && stage == 0) {
            offers++;
            stage = 1;
            length = dhcp_request(&net_out[UDP_PAYLOAD_AT], mac, xid, &lease);
            length = udp_build(net_out, &us, &server, &net_out[UDP_PAYLOAD_AT], length);
            eth_send(net_out, length);
        } else if (message == DhcpAck) {
            acks++;
            stage = 2;
        }
    }

    /* The lease becomes this machine's address here and nowhere else: `us` is
     * what every later exchange means by "who we are", and an address left in
     * the lease alone is one udp_parse() would never match against. */
    if (stage == 2) {
        memcpy(us.ip, lease.ip, IPV4_BYTES);
    }

    /* Now answer for the address just taken.  Nothing can send anything back
     * to this machine until it does: a server with a datagram for us asks who
     * has our address first, and silence there means the reply is never
     * delivered however well the rest of the stack works. */
    uint16_t served = 0;
    if (stage == 2) {
        restart_clock();
        while (frames_elapsed < SERVE_FRAMES) {
            tick();
            uint8_t rx = 0;
            /* Bounded to what an ARP exchange is on the wire rather than to
             * the buffer: eth_receive() drops what will not fit before copying
             * it, which is the cheapest way to not spend a 1500-byte DMA on
             * every neighbour's traffic.  ETH_MIN_RECEIVED and not
             * ETH_MIN_FRAME -- the reported length carries the check sequence,
             * and 60 here answered nothing at all. */
            const uint16_t got = eth_receive(net_in, ETH_MIN_RECEIVED, &rx);
            const uint16_t answer = got ? arp_answer(net_in, got, us.mac, us.ip) : 0;
            if (answer) {
                eth_send(net_in, answer);
                served++;
            }
        }
    }
    log_put16(LOG_ARP_SERVED, served);

    sector_buffer[LOG_DHCP_STAGE] = stage;
    log_put16(LOG_DHCP_OFFERS, offers);
    log_put16(LOG_DHCP_ACKS, acks);
    memcpy(&sector_buffer[LOG_LEASE + 0], lease.ip, IPV4_BYTES);
    memcpy(&sector_buffer[LOG_LEASE + 4], lease.server, IPV4_BYTES);
    memcpy(&sector_buffer[LOG_LEASE + 8], lease.router, IPV4_BYTES);
    sector_buffer[LOG_HAS_TFTP] = lease.has_tftp ? 1 : 0;
    memcpy(&sector_buffer[LOG_LEASE + 12], lease.tftp, IPV4_BYTES);

    /* The whole point: a reply to our own request, which needs the transmit
     * path, the wire, the target and the receive path all to work. */
    sector_buffer[LOG_ANSWERED] = answered ? 1 : 0;
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        sector_buffer[LOG_REPLY_MAC + i] = reply_mac[i];
    }
    log_put16(LOG_FRAMES, frames);
    log_put16(LOG_LOGGED, logged);
    sector_buffer[21] = ETHERNET.ctrl1;
    sector_buffer[22] = ETHERNET.ctrl2;

    sdcard_writesector(LOG_SECTOR, 0);

    /* Green border: the run finished and the sector is written. */
    VICIV.bordercol = 5;
    for (;;) {
        __asm__ volatile("");
    }
}
