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
 *   mega65_ftp -e -y -c "secdump out.bin <sector> 1" -c "exit"
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
constexpr uint8_t LOG_DHCP_STAGE = 40;
constexpr uint8_t LOG_DHCP_OFFERS = 41;
constexpr uint8_t LOG_DHCP_ACKS = 43;
constexpr uint8_t LOG_HAS_TFTP = 45;
constexpr uint8_t LOG_LEASE = 48;
constexpr uint8_t LOG_LEASE_FIELDS = 4;
static_assert(LOG_LEASE + LOG_LEASE_FIELDS * IPV4_BYTES <= LOG_HEADER,
    "the log header has run into the first frame slice");

/* Long enough to see ordinary LAN traffic, short enough that a run finishes
 * while the host is still waiting for it. */
constexpr uint16_t POLLS = 2000;
constexpr uint32_t POLL_US = 1000;

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

    for (uint16_t poll = 0; poll < POLLS; poll++) {
        if (poll == POLLS / 4) {
            const uint16_t length = arp_request(net_out, mac, SENDER, TARGET);
            eth_send(net_out, length);

        }

        uint8_t rx_flags = 0;
        const uint16_t got = eth_receive(net_in, sizeof net_in, &rx_flags);
        if (!got) {
            usleep(POLL_US);
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

    for (uint16_t poll = 0; poll < POLLS && stage < 2; poll++) {
        uint8_t rx = 0;
        const uint16_t got = eth_receive(net_in, sizeof net_in, &rx);
        /* A datagram too long for the buffer is not half-parsed: the headers
         * would check out and the payload would be somebody else's memory. */
        if (!got || (rx & ETH_RX_TRUNCATED)) {
            usleep(POLL_US);
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
