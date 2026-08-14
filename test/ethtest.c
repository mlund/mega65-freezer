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
#include "dma.h"
#include "eth.h"
#include "ip.h"
#include "mega65_regs.h"
#include "sdcard.h"

#include <mega65.h>

/* The first sector of ETHLOG.DAT, worked out from its cluster and the card's
 * geometry, and checked by writing a pattern through the filesystem and
 * reading this sector back. */
constexpr uint32_t LOG_SECTOR = 22532;

constexpr uint8_t FRAME_SLICE = 64; /* enough for the headers of anything */
/* Three, not four: the fourth slot would start at 256, which does not fit
 * the uint8_t the offset is held in and wraps onto the header. */
constexpr uint8_t FRAMES_LOGGED = 3;
constexpr uint8_t LOG_HEADER = 64;

/* Long enough to see ordinary LAN traffic, short enough that a run finishes
 * while the host is still waiting for it. */
constexpr uint16_t POLLS = 2000;
constexpr uint32_t POLL_US = 1000;
constexpr uint16_t UDP_FROM_PORT = 4511;
constexpr uint16_t UDP_TO_PORT = 4512;
/* A frame takes microseconds to go out; this is only a bound, not a wait. */
constexpr uint16_t TX_IDLE_POLLS = 100;

static uint8_t frame[ETH_MIN_FRAME];
/* Room for the headers and a short payload, for the UDP datagram below. */
static uint8_t out_frame[UDP_PAYLOAD_AT + 16];

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
        sector_buffer[8 + i] = mac[i];
    }
    sector_buffer[16] = ETHERNET.ctrl1;
    sector_buffer[17] = ETHERNET.ctrl2;
    sector_buffer[18] = ETHERNET.ctrl3;

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
            const uint16_t length = arp_request(frame, mac, SENDER, TARGET);
            eth_send(frame, length);

            /* And a UDP broadcast, whose checksum the listener's own kernel
             * verifies before any socket sees it -- so its arrival is an
             * account of the arithmetic that this code did not write. */
            static const uint8_t hello[] = {'M', '6', '5', 'U', 'D', 'P', '1'};
            struct NetEndpoint from = {.port = UDP_FROM_PORT};
            struct NetEndpoint to = {.port = UDP_TO_PORT};
            for (uint8_t i = 0; i < MAC_BYTES; i++) {
                from.mac[i] = mac[i];
                to.mac[i] = 0xFF; /* broadcast */
            }
            for (uint8_t i = 0; i < IPV4_BYTES; i++) {
                from.ip[i] = SENDER[i];
                to.ip[i] = 255;
            }
            const uint16_t datagram = udp_build(out_frame, &from, &to, hello, sizeof hello);
            /* Bounded, like every other wait here: a transmitter that never
             * reports idle would otherwise hang before the log is written, and
             * a hung machine says less than a log that records the fact. */
            for (uint16_t spin = 0; spin < TX_IDLE_POLLS && !eth_tx_idle(); spin++) {
                usleep(POLL_US);
            }
            sector_buffer[27] = eth_tx_idle() ? 1 : 0;
            eth_send(out_frame, datagram);
            sector_buffer[20] = 1;
        }

        uint8_t rx_flags = 0;
        const uint16_t got = eth_receive(frame, sizeof frame, &rx_flags);
        if (!got) {
            usleep(POLL_US);
            continue;
        }
        frames++;
        if (!answered && arp_reply_from(frame, got, TARGET, reply_mac)) {
            answered = true;
        }

        if (logged < FRAMES_LOGGED) {
            const uint8_t at = (uint8_t)(LOG_HEADER + logged * FRAME_SLICE);
            log_put16(at, got);
            sector_buffer[at + 2] = rx_flags;
            for (uint8_t i = 0; i < sizeof frame; i++) {
                sector_buffer[at + 4 + i] = frame[i];
            }
            logged++;
        }
    }

    /* The whole point: a reply to our own request, which needs the transmit
     * path, the wire, the target and the receive path all to work. */
    sector_buffer[26] = answered ? 1 : 0;
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        sector_buffer[32 + i] = reply_mac[i];
    }
    log_put16(4, frames);
    log_put16(6, logged);
    sector_buffer[21] = ETHERNET.ctrl1;
    sector_buffer[22] = ETHERNET.ctrl2;

    sdcard_writesector(LOG_SECTOR, 0);

    /* Green border: the run finished and the sector is written. */
    VICIV.bordercol = 5;
    for (;;) {
        __asm__ volatile("");
    }
}
