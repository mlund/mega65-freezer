/*
 * What the HTTP transport does on a real wire, with nothing else in the way.
 *
 * A plain PRG rather than a freezer tool, so etherload can push it and run it
 * with no keypresses and no menu: measuring through the browser measures the
 * browser too -- its screen, its card writes, and a serial monitor polling the
 * status line, which halts the CPU at instruction boundaries and slows the
 * very transfer being timed.
 *
 * Three passes, so the costs can be told apart: the catalogue transcoded and
 * counted, the same image summed and thrown away, and the same image again
 * onto the card.  The first two touch no storage at all, so the difference
 * between the second and the third is the staging and the card and nothing
 * else -- which through the browser is buried under its screen and its
 * status line.
 *
 *   etherload -r httpbench.prg     then read the screen at $0400
 *
 * The proxy is compiled in: a benchmark that reads a file off the card would
 * need hyppo set up, which is one of the things being avoided.
 */

#include "dma.h"
#include "fat32.h"
#include "fetch.h"
#include "helper.h"
#include "http.h"
#include "ip.h"
#include "jsoncat.h"
#include "mega65_regs.h"
#include "sdcard.h"

#include <mega65.h>
#include <string.h>

#ifndef BENCH_PROXY
#define BENCH_PROXY "46.30.215.17"
#endif
#ifndef BENCH_HOST
#define BENCH_HOST "m65filehost.twistedpair.se"
#endif

#define CATALOGUE_PATH "/php/readfilespublic.php"

/* Where the card copy goes: a file of the right length that is already there,
 * written over where it lies.  Making a new one needs a wholly free run of
 * FAT sectors, which a used card rarely has -- and the bytes written are the
 * same bytes it already held, so nothing is lost either way.  This is what the
 * browser's R does. */
#ifndef IMAGE_FILE
#define IMAGE_FILE "EASTEP30.D81"
#endif
static constexpr uint32_t IMAGE_BYTES = 819200;

static constexpr uint32_t FETCH_MAX_BYTES = 1024UL * 1024;

/* One disk image, named here because this program keeps no catalogue to pick
 * one out of.  Any .d81 the proxy serves will do; the sum below is what says
 * the bytes were right. */
#ifndef BENCH_IMAGE
#define BENCH_IMAGE "/files/e/Easter_eggs_sB7rhi.d81"
#endif

/* The C64 screen a freshly reset machine is drawing. */
#define SCREEN ((volatile uint8_t*)0x0400)
static constexpr uint8_t COLUMNS = 40;
static uint8_t row;

/* Screen codes, which for the letters and digits this prints are ASCII less
 * $40 for letters and ASCII itself for the rest. */
static void say(const char* text) {
    volatile uint8_t* at = &SCREEN[row * COLUMNS];
    for (uint8_t i = 0; i < COLUMNS; i++) {
        const char c = text[i];
        if (!c) {
            for (; i < COLUMNS; i++) {
                at[i] = ' ';
            }
            break;
        }
        at[i] = (uint8_t)(c >= '@' ? c - '@' : c);
    }
    row++;
}

static char* number(char* at, uint32_t value) {
    char digits[10];
    uint8_t n = 0;
    do {
        digits[n++] = (char)('0' + (uint8_t)(value % 10));
        value /= 10;
    } while (value);
    while (n) {
        *at++ = digits[--n];
    }
    return at;
}

static char* text_of(char* at, const char* s) {
    while (*s) {
        *at++ = *s++;
    }
    return at;
}

/* --- the seams fetch.h leaves to whoever links it --------------------- */

static struct JsonCatalog transcoder;
static uint8_t record[JSONCAT_RECORD_BYTES];
static bool transcoding;
static uint32_t sum32;
/* Nonzero while the image is going onto the card, and the file's first sector
 * when it is. */
static uint32_t store_sector;

bool fetch_cancelled(void) {
    return false;
}

bool fetch_stores_image(void) {
    return store_sector != 0;
}

bool fetch_store_blocks(uint32_t offset, const uint8_t* bytes, uint8_t count) {
    return fat32_write_file_sectors(store_sector, offset, bytes, count);
}

/* The catalogue is transcoded and counted; an image is summed and dropped.
 * Neither is kept, which is the point: what is left is the transport. */
bool fetch_store(uint32_t offset, const uint8_t* bytes, uint16_t length) {
    if (store_sector) {
        return fat32_write_file_sector(store_sector, offset, bytes, length);
    }
    (void)offset;
    if (!transcoding) {
        for (uint16_t i = 0; i < length; i++) {
            sum32 += bytes[i];
        }
        return true;
    }
    uint16_t left = length;
    while (left) {
        uint16_t taken = 0;
        if (jsoncat_take(&transcoder, bytes, left, &taken, record)) {
            /* Counted, not kept: transcoder.count is the answer. */
        }
        bytes += taken;
        left = (uint16_t)(left - taken);
    }
    return true;
}

void fetch_progress(uint32_t so_far, uint32_t total, bool last, bool waiting) {
    (void)so_far;
    (void)total;
    (void)last;
    (void)waiting;
}

/* --- the run ---------------------------------------------------------- */

/* One fetch, and its numbers on one line. */
static void bench(const char* label, const char* path) {
    uint32_t got = 0;
    const enum FetchResult result = fetch_file(path, FETCH_MAX_BYTES, &got);
    const struct FetchCounters* c = &fetch_counters;

    char line[41];
    char* at = text_of(line, label);
    if (result != FetchOk) {
        at = text_of(at, " FAILED ");
        at = number(at, result);
        *at = 0;
        say(line);
        return;
    }
    at = text_of(at, " KB=");
    at = number(at, c->bytes >> 10);
    at = text_of(at, " CS=");
    at = number(at, c->frames_total);      /* video frames, 50 per second */
    at = text_of(at, " DROP=");
    at = number(at, c->dropped);
    at = text_of(at, " RE=");
    at = number(at, c->retransmits);
    *at = 0;
    say(line);
}

int main(void) {
    __asm__ volatile("sei" ::: "memory");
    POKE(0x00, CPU_PORT_DDR_ALL_OUTPUTS);
    POKE(0x01, CPU_PORT_KERNAL_AND_IO);
    m65_io_enable();
    /* hdos_new_attach, which reading the card's directory needs. */
    mega65_dos_init();
    /* And the two a program writing sectors must set for itself, which a
     * freezer tool inherits and this does not: without the first, sdcard.c
     * addresses the card in bytes rather than sectors and the controller
     * never reports the transfer complete; without the second, a write ships
     * whatever the floppy's buffer last held.  test/sdbatch.c says the same. */
    sdcard_open();
    sdcard_visual_feedback(0);
    sdhc_card = (SDCARD.status & SD_SDHC_MASK) != 0;
    SDCARD.control |= SD_BUFFSEL_MASK;

    for (uint16_t i = 0; i < 1000; i++) {
        SCREEN[i] = ' ';
    }
    row = 0;
    say("HTTP TRANSPORT BENCHMARK");

    uint8_t ip[IPV4_BYTES];
    uint16_t port = 0;
    char address[] = BENCH_PROXY;
    if (!ip_parse(address, ip, &port)) {
        say("BAD PROXY ADDRESS");
        for (;;) {
        }
    }
    fetch_set_server(ip, port, BENCH_HOST);

    transcoding = true;
    jsoncat_begin(&transcoder);
    bench("CATALOGUE", CATALOGUE_PATH);
    transcoding = false;

    char line[41];
    char* at = text_of(line, "RECORDS=");
    at = number(at, transcoder.count);
    at = text_of(at, " SKIPPED=");
    at = number(at, transcoder.skipped);
    *at = 0;
    say(line);

    /* And an image, which is the transport with no transcoder on top of it. */
    sum32 = 0;
    bench("IMAGE    ", BENCH_IMAGE);
    at = text_of(line, "SUM=");
    at = number(at, sum32);
    *at = 0;
    say(line);

    /* And the same image again, this time onto the card: what is left over
     * the line above is the sector staging and the writes. */
    if (fat32_open_file_system() == FreezerOk) {
        store_sector = fat32_file_first_sector(IMAGE_FILE, IMAGE_BYTES);
        if (store_sector) {
            bench("TO CARD  ", BENCH_IMAGE);
            store_sector = 0;
        } else {
            say("NO " IMAGE_FILE " OF THE RIGHT SIZE ON THE CARD");
        }
    } else {
        say("CANNOT READ THE CARD");
    }

    say("DONE");
    for (;;) {
    }
}
