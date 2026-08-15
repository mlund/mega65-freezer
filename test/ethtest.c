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
 *   ...wait: the run listens, leases, fetches the catalogue, then answers ARP
 *   for a while...
 *   mega65_ftp -e -y -c "secdump out.bin <sector> 1" -c "exit"
 *
 * The sector reads BUSY until the run finishes and ETH2 once it has, so a
 * readback taken too early is obvious rather than plausible.  The border goes
 * green at the end.
 *
 * The addresses below are a LAN's, too: SENDER has to be free on it and TARGET
 * has to be something that answers.  The transfer needs no address of its own
 * here -- the lease names the TFTP server, in option 150 -- so a LAN whose
 * DHCP server says nothing about TFTP runs everything but that, and says so in
 * the log rather than inventing a server to ask.
 */

#include "arp.h"
#include "common.h"
#include "dhcp.h"
#include "dma.h"
#include "eth.h"
#include "ip.h"
#include "mega65_regs.h"
#include "netpoll.h"
#include "sdcard.h"
#include "tftp.h"

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
constexpr uint8_t LOG_HAS_TFTP = 45;
constexpr uint8_t LOG_SERVE_SEEN = 46;
constexpr uint8_t LOG_ARP_ANSWERED = 28;
constexpr uint8_t LOG_LEASE = 48;
constexpr uint8_t LOG_LEASE_FIELDS = 4;
static_assert(LOG_LEASE + LOG_LEASE_FIELDS * IPV4_BYTES <= LOG_HEADER,
    "the log header has run into the first frame slice");

/* What the transfer left behind, in the half of the sector the frame slices
 * stop short of.  Sixteen bytes of the first block because for `catalog` those
 * are the magic and the version ether65's docs/FILEHOST.md §2 fixes, so the
 * dump says not only that a transfer completed but that it brought back the
 * file it asked for. */
constexpr uint16_t LOG_TFTP = 256;
constexpr uint16_t LOG_TFTP_STAGE = LOG_TFTP + 0;
constexpr uint16_t LOG_TFTP_ERROR = LOG_TFTP + 2;
constexpr uint16_t LOG_TFTP_SERVER_MAC = LOG_TFTP + 4;
constexpr uint16_t LOG_TFTP_SIZE = LOG_TFTP + 12;
constexpr uint16_t LOG_TFTP_BYTES = LOG_TFTP + 16;
constexpr uint16_t LOG_TFTP_BLOCKS = LOG_TFTP + 20;
constexpr uint16_t LOG_TFTP_SUM = LOG_TFTP + 22;
constexpr uint16_t LOG_TFTP_FIRST = LOG_TFTP + 24;
constexpr uint8_t LOG_TFTP_FIRST_BYTES = 16;
static_assert(LOG_TFTP >= LOG_HEADER + FRAMES_LOGGED * FRAME_SLICE,
    "the transfer's fields have run into the last frame slice");
static_assert(LOG_TFTP_FIRST + LOG_TFTP_FIRST_BYTES <= SD_SECTOR_SIZE,
    "the transfer's fields have run off the end of the sector");

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
/* And long enough for a catalogue: 33 KB is 65 blocks, one outstanding at a
 * time, so what this budget really covers is the retransmissions. */
constexpr uint16_t TRANSFER_FRAMES = 300;

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
static __attribute__((section(".netbuf"))) uint8_t net_out[DHCP_FRAME_BYTES];
static __attribute__((section(".netbuf"))) uint8_t net_in[ETH_MAX_RECEIVED];

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

static void log_put16(uint16_t at, uint16_t value) {
    sector_buffer[at] = (uint8_t)value;
    sector_buffer[at + 1] = (uint8_t)(value >> 8);
}

static void log_put32(uint16_t at, uint32_t value) {
    log_put16(at, (uint16_t)value);
    log_put16((uint16_t)(at + 2), (uint16_t)(value >> 16));
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
     * length and the frame as eth_receive() delivered it -- our own frames
     * included, which the source address in the dump tells apart. */
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
        const uint16_t got = eth_receive(net_in, sizeof net_in);
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
            for (uint8_t i = 0; i < FRAME_SLICE - 2; i++) {
                sector_buffer[at + 2 + i] = net_in[i];
            }
            logged++;
        }
    }

    /* And now a lease, from whatever answers on this LAN: the exchange the
     * machine has to complete before it has an address of its own.  `us` is
     * only who we are for answering ARP -- the DHCP transport is dhcp_step()'s
     * own business now, so nothing here knows a port or a broadcast address. */
    struct NetEndpoint us = {0};
    memcpy(us.mac, mac, MAC_BYTES);

    struct DhcpClient client;
    /* Where the beam happened to be is the only thing aboard that differs
     * between two runs of a program loaded at the same address by the same
     * command.  Eight bits of it, so two runs in 256 still collide -- enough
     * to tell this run's replies from the last one's, not a random number. */
    dhcp_start(&client, mac, VICIV.fn_raster_lsb);

    restart_clock();
    /* One resend period in the past, so the first tick speaks rather than
     * waiting a quarter of the budget out.  Deliberately wrapping: the compare
     * below is a difference, so any value that far behind reads as due. */
    uint16_t said_at = (uint16_t)(0 - RESEND_FRAMES);
    uint16_t said = 0;
    while (frames_elapsed < LISTEN_FRAMES && !dhcp_leased(&client)) {
        tick();
        /* Polled every tick, not only on the ticks nothing else happens: the
         * queue is three frames deep and three arrive in 19 microseconds, so a
         * tick that skips the wire can lose the very offer it is waiting for --
         * and can leave an ARP question unanswered. */
        const uint16_t got = net_poll(net_in, sizeof net_in, &us);
        said = got ? dhcp_step(&client, net_in, got, net_out) : 0;

        /* Nothing moved for a while: say the current step again rather than
         * spend the whole budget on one lost broadcast. */
        if (!said && frames_elapsed - said_at >= RESEND_FRAMES) {
            said = dhcp_step(&client, nullptr, 0, net_out);
        }
        if (said) {
            eth_send(net_out, said);
            /* When we last spoke, not when we last timed out -- otherwise the
             * request an offer provokes leaves the clock where it was and the
             * next resend fires early. */
            said_at = frames_elapsed;
        }
    }

    /* The lease becomes this machine's address here and nowhere else: `us` is
     * what every later exchange means by "who we are", and an address left in
     * the lease alone is one udp_parse() would never match against. */
    if (dhcp_leased(&client)) {
        memcpy(us.ip, client.lease.ip, IPV4_BYTES);
    }

    /* And now the point of the lease: fetch the catalogue over TFTP from the
     * server the lease named.  Nothing is stored -- what is under test is the
     * transfer, so the blocks are counted and summed and the first sixteen
     * bytes kept, which for `catalog` are the magic that says the right file
     * came back.  Sum the same file on the server to compare:
     *
     *   python3 -c "print(sum(open('catalog','rb').read()) & 0xFFFF)"
     */
    static_assert(sizeof net_out >= TFTP_SEND_BYTES, "no room to build a request");
    static_assert(sizeof net_in >= TFTP_RECEIVE_BYTES, "a full block would be dropped");
    uint8_t server_mac[MAC_BYTES] = {0};
    if (dhcp_leased(&client) && client.lease.has_tftp) {
        /* Whose hardware address the frames must carry, which is not the
         * address they are sent to. */
        const uint8_t* hop = ip_next_hop(
            client.lease.tftp, us.ip, client.lease.netmask, client.lease.router);
        restart_clock();
        uint16_t asked_at = (uint16_t)(0 - RESEND_FRAMES);
        while (frames_elapsed < LISTEN_FRAMES && net_zero(server_mac, MAC_BYTES)) {
            tick();
            if (frames_elapsed - asked_at >= RESEND_FRAMES) {
                eth_send(net_out, arp_request(net_out, mac, us.ip, hop));
                asked_at = frames_elapsed;
            }
            const uint16_t got = net_poll(net_in, ETH_MIN_RECEIVED, &us);
            if (got) {
                (void)arp_reply_from(net_in, got, hop, server_mac);
            }
        }
    }

    struct TftpClient transfer = {0};
    uint16_t blocks = 0;
    uint32_t fetched = 0;
    uint16_t sum = 0;
    if (!net_zero(server_mac, MAC_BYTES)) {
        struct NetEndpoint server = {0};
        memcpy(server.mac, server_mac, MAC_BYTES);
        memcpy(server.ip, client.lease.tftp, IPV4_BYTES);
        tftp_start(&transfer, &us, &server, "catalog", VICIV.fn_raster_lsb);

        restart_clock();
        uint16_t said = tftp_step(&transfer, nullptr, 0, net_out);
        uint16_t said_at = 0;
        while (frames_elapsed < TRANSFER_FRAMES && !tftp_done(&transfer)
            && !tftp_failed(&transfer)) {
            tick();
            if (said) {
                eth_send(net_out, said);
                said_at = frames_elapsed;
            }
            /* net_poll() and not eth_receive(): this machine has an address
             * now, and a transfer is exactly when the server asks who holds
             * it -- an unanswered question there stops the blocks arriving
             * however well the rest of this works.  The limit is the largest
             * frame the transfer can be, so the LAN's other traffic is refused
             * before the DMA rather than after. */
            const uint16_t got = net_poll(net_in, TFTP_RECEIVE_BYTES, &us);
            said = got ? tftp_step(&transfer, net_in, got, net_out) : 0;
            if (!said && frames_elapsed - said_at >= RESEND_FRAMES) {
                said = tftp_step(&transfer, nullptr, 0, net_out);
            }
            if (transfer.data_length) {
                if (!blocks) {
                    for (uint8_t i = 0; i < LOG_TFTP_FIRST_BYTES; i++) {
                        sector_buffer[LOG_TFTP_FIRST + i] = net_in[TFTP_DATA_AT + i];
                    }
                }
                blocks++;
                fetched += transfer.data_length;
                for (uint16_t i = 0; i < transfer.data_length; i++) {
                    sum = (uint16_t)(sum + net_in[TFTP_DATA_AT + i]);
                }
            }
        }
        /* The acknowledgement the last block earned: the loop above ends on it
         * rather than sending it, and a server that never hears it retries the
         * whole of its last block and records a failure for a file that
         * arrived whole. */
        if (said) {
            eth_send(net_out, said);
        }
    }
    log_put16(LOG_TFTP_STAGE, transfer.stage);
    log_put16(LOG_TFTP_ERROR, transfer.error);
    memcpy(&sector_buffer[LOG_TFTP_SERVER_MAC], server_mac, MAC_BYTES);
    log_put32(LOG_TFTP_SIZE, transfer.size);
    log_put32(LOG_TFTP_BYTES, fetched);
    log_put16(LOG_TFTP_BLOCKS, blocks);
    log_put16(LOG_TFTP_SUM, sum);

    /* Now answer for the address just taken.  Nothing can send anything back
     * to this machine until it does: a server with a datagram for us asks who
     * has our address first, and silence there means the reply is never
     * delivered however well the rest of the stack works. */
    uint16_t seen = 0;
    if (dhcp_leased(&client)) {
        restart_clock();
        while (frames_elapsed < SERVE_FRAMES) {
            tick();
            /* Bounded to what an ARP exchange is on the wire rather than to
             * the buffer, which is what a small `limit` is for.
             * ETH_MIN_RECEIVED and not ETH_MIN_FRAME -- the reported length
             * carries the check sequence, and 60 here answered nothing at all.
             *
             * The answering itself is net_poll()'s, and what proves it is a
             * ping of the leased address from the other end during this
             * window: an unanswered question means no reply is ever delivered,
             * so a ping that comes back is the whole test.  What is counted
             * here is the traffic that was not that. */
            seen += net_poll(net_in, ETH_MIN_RECEIVED, &us) ? 1 : 0;
        }
    }
    log_put16(LOG_SERVE_SEEN, seen);
    log_put16(LOG_ARP_ANSWERED, net_answers());

    /* The stage is the whole account of how far the exchange got: still
     * discovering means nothing offered, still requesting means an offer that
     * was never acknowledged. */
    sector_buffer[LOG_DHCP_STAGE] = client.stage;
    memcpy(&sector_buffer[LOG_LEASE + 0], client.lease.ip, IPV4_BYTES);
    memcpy(&sector_buffer[LOG_LEASE + 4], client.lease.server, IPV4_BYTES);
    memcpy(&sector_buffer[LOG_LEASE + 8], client.lease.router, IPV4_BYTES);
    sector_buffer[LOG_HAS_TFTP] = client.lease.has_tftp ? 1 : 0;
    memcpy(&sector_buffer[LOG_LEASE + 12], client.lease.tftp, IPV4_BYTES);

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
