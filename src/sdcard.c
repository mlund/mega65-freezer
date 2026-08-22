/* Working the SD controller: open the card, move a sector through the one
 * buffer the hardware offers, and say on the border that it is busy. */

#include "sdcard.h"

#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"

#include <mega65.h>
#include <stdio.h>
#include <stdlib.h>

static constexpr Addr28 SD_SECTORBUFFER = 0xffd6e00;

unsigned char sdhc_card = 0;
uint8_t border_flicker = 0;

/* Polls, not time: how long a read waits before the card counts as stalled.
 * The controller answers in microseconds when it is well. */
static constexpr uint16_t SD_READY_POLLS = 50000;
/* A card that is not SDHC is addressed by byte, and sector * SD_SECTOR_SIZE has to stay
 * inside 32 bits, so the sector number stops here. */
static constexpr uint32_t SD_MAX_SECTOR_BYTE_ADDRESSED = 0x7FFFFF;

/* Whether the border cycles while the controller is busy: 0 silent, above 1
 * reports.  1 is accepted and does nothing -- no site tests for it -- so a
 * caller that wants to be told anything asks for 2. */
void sdcard_visual_feedback(const uint8_t do_flicker) {
    border_flicker = do_flicker < 3 ? do_flicker : 2;
}

void sdcard_reset(void) {
    SDCARD.command = SDCARD_RESET_BEGIN;
    SDCARD.command = SDCARD_RESET_END;

    // Now wait for SD card reset to complete
    while (SDCARD.status & SD_STATUS_BUSY) {
        if (border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
    }

    if (sdhc_card) {
        SDCARD.command = SDCARD_SDHC_MODE_ON;
    }
}

void mega65_fast(void) {
    CPU_PORTDDR = CPU_PORT_FORCE_FAST;
    // MEGA65 IO registers
    VICIV.key = VIC4_KEY_VICIV_A;
    VICIV.key = VIC4_KEY_VICIV_B;
}

void sdcard_open(void) {
    sdcard_reset();
}

#ifdef SDCARD_COUNTERS
/* Counting, not timing.  An emulated card completes a transaction in no time,
 * so the number of transactions is the only figure that carries across to
 * hardware; multiply it there by a per-operation cost measured on the card.
 *
 * Every command issued to the controller is counted, not every call: a sector
 * that has to be retried costs the card twice and should read as twice. */
uint32_t sd_reads = 0;
uint32_t sd_writes = 0;
uint32_t sd_writes_skipped = 0; /* sector already held what we were about to write */
uint32_t sd_write_failures = 0; /* sectors the card would not take */

/* Where a slow sector's time goes, in polls rather than in any clock: the
 * waits below already count them and a poll costs the same every time, so the
 * figure is proportional to the pause and needs no timer this machine does not
 * have.  Only the three waits are watched -- the DMA is 512 bytes and the
 * compare stops at the first difference, so neither can vary by seconds.
 *
 * Maxima, with the sector that set the worst of them, because the question is
 * which single sector took five seconds rather than what the average is.  The
 * sector number is the test for a slow card region: the same one across two
 * runs of different files means the card, not the driver. */
uint32_t sd_polls_before_read = 0;
uint32_t sd_polls_before_write = 0;
uint32_t sd_polls_after_write = 0;
/* Volatile: nothing in the program reads it back, so link-time optimisation
 * proves the stores dead and drops the name the harness asks for. */
volatile uint32_t sd_slowest_sector = 0;
uint32_t sd_slowest_polls = 0;

/* And in frames, the only figure that answers "seconds": polls are
 * proportional to time but not convertible to it, 562052 of them being a few
 * hundred milliseconds rather than the five seconds a server waits.
 *
 * Sampled from inside the wait -- a caller cannot sample a call it is blocked
 * in, and a wait spanning several frames looks like one from outside.  Every
 * 4096 polls, finer than a frame at one test in four thousand.
 *
 * `worst` is the longest single wait, `total` every frame spent waiting: a
 * small first and large second means the cost is spread, not stalling. */
uint32_t sd_frames_worst = 0;
uint32_t sd_frames_total = 0;

#define DEBUG_COUNT(counter) ((counter)++)
#else
#define DEBUG_COUNT(counter) ((void)0)
#endif

#ifdef SDCARD_COUNTERS
/* What the last wait cost, for the caller to file under its own phase. */
static uint32_t sd_last_polls;
#define POLLS_SPENT(round, spins) (sd_last_polls = ((uint32_t)(round) << 16) | (spins))
#define FRAMES_SPENT(frames)                                                                       \
    do {                                                                                           \
        sd_frames_total += (frames);                                                               \
        if ((frames) > sd_frames_worst) {                                                          \
            sd_frames_worst = (frames);                                                            \
        }                                                                                          \
    } while (0)
#define PHASE_MAX(phase, sector)                                                                   \
    do {                                                                                           \
        if (sd_last_polls > (phase)) {                                                             \
            (phase) = sd_last_polls;                                                               \
        }                                                                                          \
        if (sd_last_polls > sd_slowest_polls) {                                                    \
            sd_slowest_polls = sd_last_polls;                                                      \
            sd_slowest_sector = (sector);                                                          \
        }                                                                                          \
    } while (0)
#else
#define POLLS_SPENT(round, spins) ((void)0)
#define PHASE_MAX(phase, sector) ((void)0)
#define FRAMES_SPENT(frames) ((void)0)
#endif

void sdcard_readsector(const uint32_t sector_number) {
    uint8_t tries = 0;
    uint16_t timeout;

    if (!sdhc_card && sector_number >= SD_MAX_SECTOR_BYTE_ADDRESSED) {
        /* Nothing sensible left to do, so stop rather than read the wrong
         * sector. */
        for (;;) {
        }
    }
    const uint32_t sector_address = sdhc_card ? sector_number : sector_number * SD_SECTOR_SIZE;

    SDCARD.sector_number = sector_address;

    while (tries < 10) {
        DEBUG_COUNT(sd_reads);

        // Wait for SD card to be ready
        timeout = SD_READY_POLLS;
        while (SDCARD.status & SD_STATUS_BUSY) {
            timeout--;
            if (!timeout) {
                // Time out -- so reset SD card
                SDCARD.command = SDCARD_RESET_BEGIN;
                SDCARD.command = SDCARD_RESET_END;
                timeout = SD_READY_POLLS;
            }
            if (SDCARD.status & SD_ERROR_MASK) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (SDCARD.status == SD_SDIO_BUSY_MASK) {
                return;
            }
        }

        SDCARD.command = SDCARD_READ_SECTOR;

        // Wait for read to complete
        timeout = SD_READY_POLLS;
        while (SDCARD.status & SD_STATUS_BUSY) {
            timeout--;
            if (!timeout) {
                return;
            }
            if (SDCARD.status & SD_ERROR_MASK) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (SDCARD.status == SD_SDIO_BUSY_MASK) {
                return;
            }
        }

        if (!(SDCARD.status & SD_STATUS_UNSETTLED)) {
            // Copy data from hardware sector buffer via DMA
            lcopy_in(SD_SECTORBUFFER, sector_buffer, SD_SECTOR_SIZE);

            return;
        }

        if (border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }

        // Reset SD card
        sdcard_open();

        tries++;
    }
}

/* src/link.ld puts .sdverify above the region the linker allocates from, so
 * this does not come out of the same budget as the code. */
static __attribute__((section(".sdverify"))) uint8_t sd_verify_buffer[SD_SECTOR_SIZE];

[[nodiscard]] static bool sdcard_ready(void);

#ifdef SDCARD_FAULTS
/* Ready checks to answer "not ready" to, counted down as they are spent.
 *
 * Poked by name from the test harness, because no emulator can stage the thing
 * this guards against: Xemu's card answers at once and never stays busy, so a
 * wait that never returns looks identical to one that returns immediately.
 * One refusal is enough: a write that cannot have the card gives up. */
/* Volatile because nothing in the program writes it: the harness pokes the
 * symbol from outside, and link-time optimisation would otherwise prove the
 * value can only be its initialiser, fold the test away and drop the name. */
volatile uint8_t sd_refuse_ready = 0;
#endif

/* Whether the card holds what we were about to write.  One copy: the write
 * asks before writing and again after, and the loop is 512 bytes either way. */
[[nodiscard]] __attribute__((noinline)) static bool verify_matches(void) {
    for (uint16_t i = 0; i < SD_SECTOR_SIZE; i++) {
        if (sector_buffer[i] != sd_verify_buffer[i]) {
            return false;
        }
    }
    return true;
}

/* The card's readiness, or a refusal the harness asked for. */
[[nodiscard]] static bool ready_or_refuse(void) {
#ifdef SDCARD_FAULTS
    if (sd_refuse_ready) {
        sd_refuse_ready--;
        return false;
    }
#endif
    return sdcard_ready();
}

/* A write gives up rather than resetting the controller and asking again.
 *
 * The reset went in against a card that would not answer, and two hardware
 * runs of more than 1600 real writes never once reached it -- sd_recoveries
 * stayed at 0 throughout.  mega65-tools' remotesd_eth.c waits and never resets
 * either.  So the card is given the time and then the caller is told, which is
 * what it can act on; resetting the controller under an operation it is still
 * performing is what the unbounded version did for ever. */

bool sdcard_writesector(const uint32_t sector_number, uint8_t is_multi) {
    uint8_t tries = 0;
    const uint32_t sector_address = sdhc_card ? sector_number : sector_number * SD_SECTOR_SIZE;

    // Set address to read/write
    SDCARD.command = SDCARD_RESET_END;
    SDCARD.sector_number = sector_address;

    /* Read the sector first and write only if it differs.  This looks like
     * waste on a bulk write and is not: measured on hardware over an 800KB
     * image, 14.5 seconds as it stands against 57.6 with the compare skipped,
     * and that run did not finish.  The compare exits at the first differing
     * byte so it costs almost nothing when it fails, and where it succeeds it
     * saves a whole write.  It also separates the read from the write, which
     * the controller is timing-sensitive enough to need. */

    DEBUG_COUNT(sd_reads);
    SDCARD.command = SDCARD_READ_SECTOR;
    if (!ready_or_refuse()) {
        DEBUG_COUNT(sd_write_failures);
        return false;
    }
    PHASE_MAX(sd_polls_before_read, sector_number);

    // Copy the read data to a buffer for verification
    lcopy_in(SD_SECTORBUFFER, sd_verify_buffer, SD_SECTOR_SIZE);

    if (verify_matches()) {
        DEBUG_COUNT(sd_writes_skipped);
        return true;
    }

    while (tries < 10) {
        DEBUG_COUNT(sd_writes);

        // Copy data to hardware sector buffer via DMA
        lcopy_out(sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);

        if (!ready_or_refuse()) {
            DEBUG_COUNT(sd_write_failures);
            return false;
        }
        PHASE_MAX(sd_polls_before_write, sector_number);

        SDCARD.command = SDCARD_WRITE_GATE;
        if (is_multi) {
            SDCARD.command = SDCARD_WRITE_MULTI_FIRST;
        } else {
            SDCARD.command = SDCARD_WRITE_SECTOR;
        }

        // Wait for the write to complete
        if (!ready_or_refuse()) {
            DEBUG_COUNT(sd_write_failures);
            return false;
        }
        PHASE_MAX(sd_polls_after_write, sector_number);

        if (!(SDCARD.status & SD_STATUS_UNSETTLED)) {
            /* The controller misbehaves unless a read follows a write, and
             * sometimes wants a reset even then; the read below is also what
             * verifies the write took. */
            DEBUG_COUNT(sd_reads);
            SDCARD.command = SDCARD_READ_SECTOR;
            if (!ready_or_refuse()) {
                DEBUG_COUNT(sd_write_failures);
                return false;
            }

            // Copy the read data to a buffer for verification
            lcopy_in(SD_SECTORBUFFER, sd_verify_buffer, SD_SECTOR_SIZE);

            if (verify_matches()) {
                return true;
            }
            screen_hex(screen_line_address - 80 + 24, sector_number);
        }

        if (border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
        /* Without this the bound is never approached and a sector that will not
         * verify is rewritten for ever. */
        tries++;
    }
    /* Ten writes that would not verify.  The caller is told, rather than left
     * to discover it from a file that is wrong. */
    DEBUG_COUNT(sd_write_failures);
    return false;
}

/* Both engines idle, or the budget spent.  Bounded because the alternative was
 * measured: a wait for a bit that never changes has no way out and no way to
 * say so, and it hung the machine mid-transfer with nothing but RESTORE left.
 * Generous, though -- a card part way through an internal erase can be busy for
 * a long time, and giving up early costs a sector where waiting costs seconds
 * at worst. */
static constexpr uint8_t SD_READY_ROUNDS = 200;
[[nodiscard]] static bool sdcard_ready(void) {
#ifdef SDCARD_COUNTERS
    uint8_t high = VICIV.fn_raster_msb & 0b00000111;
    uint16_t frames = 0;
#endif
    for (uint8_t round = 0; round < SD_READY_ROUNDS; round++) {
        if (border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
        uint16_t spins = 0;
        do {
            if (!(SDCARD.status & SD_STATUS_BUSY)) {
                POLLS_SPENT(round, spins);
                FRAMES_SPENT(frames);
                return true;
            }
#ifdef SDCARD_COUNTERS
            if (!(spins & 0x0FFF)) {
                const uint8_t now = VICIV.fn_raster_msb & 0b00000111;
                if (now < high) {
                    frames++;
                }
                high = now;
            }
#endif
        } while (++spins);
    }
    POLLS_SPENT(SD_READY_ROUNDS, 0);
    FRAMES_SPENT(frames);
    return false;
}

/* Opens a multi-block write and sends its first block.
 *
 * Separate from sdcard_writesector(), which cannot serve: it reads the sector
 * back, and sdcardio.vhdl shows what other traffic does to an open stream --
 * command $03 clears sd_write_multi outright.  Measured on hardware, the first
 * block landed and every one after was lost.  It also short-circuits on an
 * unchanged sector, leaving the stream unopened.
 *
 * So this verifies nothing; the batch is read back once closed, as
 * mega65-tools' remotesd_eth.c does. */
bool sdcard_writefirstsector(const uint32_t sector_number) {
    const uint32_t sector_address = sdhc_card ? sector_number : sector_number * SD_SECTOR_SIZE;
    SDCARD.command = SDCARD_RESET_END;
    SDCARD.sector_number = sector_address;

    if (!sdcard_ready()) {
        return false;
    }
    lcopy_out(sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);
    DEBUG_COUNT(sd_writes);
    SDCARD.command = SDCARD_WRITE_GATE;
    SDCARD.command = SDCARD_WRITE_MULTI_FIRST;
    return true;
}

/* A follow-on block of an open CMD25 stream: the card holds the address, so
 * only the data and the command are needed.
 *
 * The command is issued and left, rather than waited on.  Waiting for BUSY to
 * *rise* is what the first version did, and a command the controller does not
 * take never raises it -- measured on hardware, where the stream's first sector
 * landed and every one after it was lost to that spin.  mega65-tools'
 * remotesd_eth.c does not wait either: it settles the card before the next
 * block instead, which is what the call above does. */
bool sdcard_writenextsector(void) {
    if (!sdcard_ready()) {
        return false;
    }
    lcopy_out(sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);
    DEBUG_COUNT(sd_writes);
    SDCARD.command = SDCARD_WRITE_GATE;
    SDCARD.command = SDCARD_WRITE_MULTI_NEXT;
    return true;
}

/* The stream's last block, which carries data like any other and closes the
 * stream behind it.  Closing is not optional: remotesd_eth.c's
 * abort_write_batch() records that a CMD25 left open makes the next write
 * fail, whatever that write is. */
bool sdcard_writelastsector(void) {
    if (!sdcard_ready()) {
        return false;
    }
    lcopy_out(sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);
    DEBUG_COUNT(sd_writes);
    SDCARD.command = SDCARD_WRITE_GATE;
    SDCARD.command = SDCARD_WRITE_MULTI_LAST;
    return sdcard_ready();
}

/* A failed CMD25 follow-on leaves its state unknown.  The controller's reset
 * pair is its documented stream escape; it releases the card without sending
 * invented data as a final block would. */
void sdcard_writeabort(void) {
    SDCARD.command = SDCARD_RESET_BEGIN;
    SDCARD.command = SDCARD_RESET_END;
}
