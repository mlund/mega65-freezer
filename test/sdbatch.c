/*
 * What a multi-sector write costs against one sector at a time.
 *
 * sdcard_writenextsector() and sdcard_writemultidone() have been declared and
 * implemented since the card routines were written and never once executed.
 * They issue CMD25 -- the card stays in a write stream, the address is set
 * once, and no read happens between sectors -- which is what mega65_ftp's
 * helper does to reach 200KB/s where fetch_file() manages 26.
 *
 * So this writes the same sectors both ways and counts the frames.  It runs
 * standalone under `etherload -r sdbatch.prg`, with nobody at the keyboard,
 * and leaves its findings in the last sector of the file it writes into --
 * fetch that file back with mega65_ftp and read the tail.
 *
 * Nothing is guessed about where to write.  The file is created here through
 * the same allocator FILEHOST uses, so the sectors belong to it and to nothing
 * else on the card.
 *
 * Each pass writes different bytes on purpose.  Writing a sector the card
 * already holds is skipped by the compare in sdcard_writesector(), and a
 * benchmark that repeated itself would measure the compare rather than the
 * write -- which is exactly how an 800KB fetch came to look four times faster
 * than it is.
 */

#include "dma.h"
#include "fat32.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"

#include <mega65.h>
#include <stdint.h>

/* One CMD25 stream's worth, and what megatalk's helper settled on:
 * WRITE_BATCH_SIZE in ether65/megatalk/src/sd/mod.rs is 128 sectors. */
static constexpr uint16_t BATCH_SECTORS = 128;
/* Enough passes that the frame counter has something to say: a batch that
 * lands near the reference speed is a third of a second, which is sixteen
 * frames, and one pass of that would round badly. */
static constexpr uint8_t PASSES = 4;
/* The data sectors, plus one at the end for what this found. */
static constexpr uint32_t FILE_SECTORS = BATCH_SECTORS + 1;
static const char TEST_FILE[] = "SDBATCH.DAT";

/* Frames, counted the way fetch.c counts them: the physical raster's high bits
 * read 0 below line 256 and 1 above it, so a 1 -> 0 step is a frame boundary.
 * $D012 wraps twice a frame and the two registers cannot be read at the same
 * instant, so neither serves. */
static constexpr uint8_t RASTER_HIGH = 0x07;
static uint8_t last_high;
static uint16_t frames;

static void tick(void) {
    const uint8_t now = VICIV.fn_raster_msb & RASTER_HIGH;
    if (now < last_high) {
        frames++;
    }
    last_high = now;
}

static void start_clock(void) {
    last_high = VICIV.fn_raster_msb & RASTER_HIGH;
    frames = 0;
}

/* Bytes no earlier pass wrote, so the compare in sdcard_writesector() cannot
 * skip the write and call it fast. */
static void fill_sector(uint8_t pass, uint16_t index) {
    for (uint16_t i = 0; i < SD_SECTOR_SIZE; i++) {
        sector_buffer[i] = (uint8_t)(pass * 37 + index * 11 + i);
    }
}

/* Whether the sectors hold what the last pass put there.  Read back through
 * the ordinary single-sector path, which is trusted: it is the multi-sector
 * write being examined, not the read. */
static bool sectors_hold(uint32_t first, uint8_t pass) {
    for (uint16_t index = 0; index < BATCH_SECTORS; index++) {
        sdcard_readsector(first + index);
        for (uint16_t i = 0; i < SD_SECTOR_SIZE; i++) {
            if (sector_buffer[i] != (uint8_t)(pass * 37 + index * 11 + i)) {
                return false;
            }
        }
    }
    return true;
}

/* One sector at a time, as fetch_file() writes today. */
static uint16_t time_single(uint32_t first, uint8_t pass) {
    start_clock();
    for (uint16_t index = 0; index < BATCH_SECTORS; index++) {
        fill_sector(pass, index);
        (void)sdcard_writesector(first + index, 0);
        tick();
    }
    return frames;
}

/* The whole run as one CMD25 stream: the address is set by the first call, the
 * middle sectors carry none, and the last closes the stream.  Leaving it open
 * makes the next write fail -- remotesd_eth.c's abort_write_batch() says so --
 * which is why the done call is not conditional on anything. */
static uint16_t time_batch(uint32_t first, uint8_t pass) {
    start_clock();
    fill_sector(pass, 0);
    (void)sdcard_writefirstsector(first);
    /* The last block carries data and closes the stream, as the middle ones
     * carry data and do not -- remotesd_eth.c sends command 4, then 5 for
     * everything between, then 6. */
    for (uint16_t index = 1; index < BATCH_SECTORS - 1; index++) {
        fill_sector(pass, index);
        if (!sdcard_writenextsector()) {
            break;
        }
        tick();
    }
    fill_sector(pass, BATCH_SECTORS - 1);
    (void)sdcard_writelastsector();
    tick();
    return frames;
}

static char* say(char* at, const char* text) {
    while (*text) {
        *at++ = *text++;
    }
    return at;
}

static char* say_num(char* at, uint16_t value) {
    char digits[6];
    uint8_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) {
        *at++ = digits[--n];
    }
    return at;
}

int main(void) {
    uint16_t single[PASSES];
    uint16_t batch[PASSES];
    uint8_t verified = 0;

    __asm__ volatile("sei" ::: "memory");
    POKE(0x00, CPU_PORT_DDR_ALL_OUTPUTS);
    POKE(0x01, CPU_PORT_KERNAL_AND_IO);
    m65_io_enable();
    sdcard_open();
    sdcard_visual_feedback(0);
    /* The two a program writing sectors must set for itself, as
     * src/freezer/main.c does.  Without the first, sdcard.c addresses the
     * card in bytes rather than sectors and the controller never reports the
     * transfer complete; without the second, a write ships whatever the
     * floppy's buffer last held, both planes living at $FFD6E00. */
    sdhc_card = (SDCARD.status & SD_SDHC_MASK) != 0;
    SDCARD.control |= SD_BUFFSEL_MASK;
    if (fat32_open_file_system() != FreezerOk) {
        return 1;
    }

    /* Created rather than found: a file this test made is one whose sectors
     * nothing else on the card is using. */
    const uint32_t first = fat32_create_contiguous_file(TEST_FILE, FILE_SECTORS * SD_SECTOR_SIZE);
    if (!first) {
        return 1;
    }

    for (uint8_t pass = 0; pass < PASSES; pass++) {
        single[pass] = time_single(first, (uint8_t)(pass * 2 + 1));
        batch[pass] = time_batch(first, (uint8_t)(pass * 2 + 2));
        /* Verified after the batch, not during it, which is what both
         * references do: mega65_ftp's helper reads the whole run back once the
         * stream is closed. */
        if (sectors_hold(first, (uint8_t)(pass * 2 + 2))) {
            verified++;
        }
    }

    /* The findings, as text, in the sector after the data.  A whole file comes
     * back with `mega65_ftp -c "get SDBATCH.DAT"`, so nothing here needs to
     * know a sector number the host would have to work out again. */
    clear_sector_buffer();
    char* at = say((char*)sector_buffer, "SDBATCH ");
    at = say_num(at, BATCH_SECTORS);
    at = say(at, " SECTORS, FRAMES SINGLE/BATCH:");
    for (uint8_t pass = 0; pass < PASSES; pass++) {
        at = say(at, " ");
        at = say_num(at, single[pass]);
        at = say(at, "/");
        at = say_num(at, batch[pass]);
    }
    at = say(at, "  VERIFIED ");
    at = say_num(at, verified);
    at = say(at, " OF ");
    at = say_num(at, PASSES);
    *at = 0;
    (void)sdcard_writesector(first + BATCH_SECTORS, 0);

    return 0;
}
