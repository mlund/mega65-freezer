/*
 * What the HTTP transport does on a real wire, with nothing else in the way.
 *
 * A plain PRG rather than a freezer tool, so etherload can push it and run it
 * with no keypresses and no menu: measuring through the browser measures the
 * browser too -- its screen, its card writes, and a serial monitor polling the
 * status line, which halts the CPU at instruction boundaries and slows the
 * very transfer being timed.
 *
 * Nothing is written to the card.  The catalogue is transcoded and counted,
 * the image is summed and thrown away, so what is left is the wire, the
 * checksums and the transcoder.
 *
 *   etherload -r httpbench.prg     then read the screen at $0400
 *
 * The proxy is compiled in: a benchmark that reads a file off the card would
 * need hyppo set up, which is one of the things being avoided.
 */

#include "dma.h"
#include "fetch.h"
#include "http.h"
#include "ip.h"
#include "jsoncat.h"
#include "mega65_regs.h"

#include <mega65.h>
#include <string.h>

#ifndef BENCH_PROXY
#define BENCH_PROXY "46.30.215.17"
#endif
#ifndef BENCH_HOST
#define BENCH_HOST "m65filehost.twistedpair.se"
#endif

#define CATALOGUE_PATH "/php/readfilespublic.php"

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

bool fetch_cancelled(void) {
    return false;
}

bool fetch_stores_image(void) {
    return false;
}

bool fetch_store_blocks(uint32_t offset, const uint8_t* bytes, uint8_t count) {
    (void)offset;
    (void)bytes;
    (void)count;
    return false;
}

/* The catalogue is transcoded and counted; an image is summed and dropped.
 * Neither is kept, which is the point: what is left is the transport. */
bool fetch_store(uint32_t offset, const uint8_t* bytes, uint16_t length) {
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
    m65_io_enable();

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

    say("DONE");
    for (;;) {
    }
}
