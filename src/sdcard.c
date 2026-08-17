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

/* Half a second for the card to come back after a reset. */
static constexpr uint32_t SD_RESET_SETTLE_MICROSECS = 500000;
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
    SD_COMMAND = SD_CMD_RESET_BEGIN;
    SD_COMMAND = SD_CMD_RESET_END;

    // Now wait for SD card reset to complete
    while (SD_STATUS & SD_STATUS_BUSY) {
        if (border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
    }

    if (sdhc_card) {
        SD_COMMAND = SD_CMD_SDHC_MODE;
    }
}

void mega65_fast(void) {
    CPU_PORTDDR = CPU_PORT_FORCE_FAST;
    // MEGA65 IO registers
    VICIV.key = VIC4_KNOCK_1;
    VICIV.key = VIC4_KNOCK_2;
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
uint32_t sd_recoveries = 0;     /* writes where the card had to be reset first */
#define DEBUG_COUNT(counter) ((counter)++)
#else
#define DEBUG_COUNT(counter) ((void)0)
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

    SD_SECTOR_ADDR(0) = (sector_address >> 0) & 0xff;
    SD_SECTOR_ADDR(1) = (sector_address >> 8) & 0xff;
    SD_SECTOR_ADDR(2) = (sector_address >> 16) & 0xff;
    SD_SECTOR_ADDR(3) = (sector_address >> 24) & 0xff;

    while (tries < 10) {
        DEBUG_COUNT(sd_reads);

        // Wait for SD card to be ready
        timeout = SD_READY_POLLS;
        while (SD_STATUS & SD_STATUS_BUSY) {
            timeout--;
            if (!timeout) {
                // Time out -- so reset SD card
                SD_COMMAND = SD_CMD_RESET_BEGIN;
                SD_COMMAND = SD_CMD_RESET_END;
                timeout = SD_READY_POLLS;
            }
            if (SD_STATUS & SD_STATUS_ERROR) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (SD_STATUS == SD_STATUS_SDIO_BUSY) {
                return;
            }
        }

        SD_COMMAND = SD_CMD_READ;

        // Wait for read to complete
        timeout = SD_READY_POLLS;
        while (SD_STATUS & SD_STATUS_BUSY) {
            timeout--;
            if (!timeout) {
                return;
            }
            if (SD_STATUS & SD_STATUS_ERROR) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (SD_STATUS == SD_STATUS_SDIO_BUSY) {
                return;
            }
        }

        if (!(SD_STATUS & SD_STATUS_UNSETTLED)) {
            // Copy data from hardware sector buffer via DMA
            lcopy(SD_SECTORBUFFER, (Addr28)sector_buffer, SD_SECTOR_SIZE);

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
 * One refusal exercises the recovery below; two exercise the failure it cannot
 * rescue. */
/* Volatile because nothing in the program writes it: the harness pokes the
 * symbol from outside, and link-time optimisation would otherwise prove the
 * value can only be its initialiser, fold the test away and drop the name. */
volatile uint8_t sd_refuse_ready = 0;
#endif

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

/* Waits for the card before a write, with one reset if it does not come.
 *
 * Bounded, and one attempt rather than for ever: this loop resetting and
 * reissuing every half second without limit is what turned a long busy into a
 * permanent wedge, sixty seconds of silence at a single block with nothing but
 * RESTORE left.  Whether the reset is needed at all is an open question --
 * mega65-tools' remotesd_eth.c never resets, and waits alone -- so it is
 * counted, and a run that never fires it is grounds for dropping it. */
[[nodiscard]] static bool wait_before_write(uint8_t is_multi) {
    if (ready_or_refuse()) {
        return true;
    }
    DEBUG_COUNT(sd_recoveries);
    SD_COMMAND = SD_CMD_RESET_BEGIN;
    usleep(SD_RESET_SETTLE_MICROSECS);
    SD_COMMAND = SD_CMD_RESET_END;
    SD_COMMAND = SD_CMD_WRITE_GATE;
    SD_COMMAND = is_multi ? SD_CMD_WRITE_MULTI_FIRST : SD_CMD_WRITE;
    return ready_or_refuse();
}

bool sdcard_writesector(const uint32_t sector_number, uint8_t is_multi) {
    uint8_t tries = 0;
    const uint32_t sector_address = sdhc_card ? sector_number : sector_number * SD_SECTOR_SIZE;

    // Set address to read/write
    SD_COMMAND = SD_CMD_RESET_END;
    SD_SECTOR_ADDR(0) = (sector_address >> 0) & 0xff;
    SD_SECTOR_ADDR(1) = (sector_address >> 8) & 0xff;
    SD_SECTOR_ADDR(2) = (sector_address >> 16) & 0xff;
    SD_SECTOR_ADDR(3) = (sector_address >> 24) & 0xff;

    /* Read the sector first and write only if it differs.
     *
     * This looks like waste on a bulk write of fresh data -- a read and a
     * 512-byte compare per sector that cannot match -- and it is not.  Measured
     * on hardware, fetching an 800KB disk image over TFTP: 14.5 seconds as it
     * stands, against 57.6 with the compare skipped, and that run did not
     * finish.  Two reasons.  The compare exits at the first differing byte, so
     * it costs almost nothing when it fails; and where it succeeds it saves a
     * whole write, which is most sectors when a file is written over its own
     * previous contents.  The loop also separates the read from the write, and
     * the controller is timing-sensitive enough that removing it makes the
     * read-back below fail and the write be retried.
     */

    DEBUG_COUNT(sd_reads);
    SD_COMMAND = SD_CMD_READ;

    /* The count wrapping is the timeout: 65536 polls without the card coming
     * ready, and it is reset and asked again. */
    uint16_t counter = 0;
    while (SD_STATUS & SD_STATUS_BUSY) {
        if (border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
        if (!++counter) {
            SD_COMMAND = SD_CMD_RESET_BEGIN;
            usleep(SD_RESET_SETTLE_MICROSECS);
            SD_COMMAND = SD_CMD_RESET_END;
            SD_COMMAND = SD_CMD_READ;
        }
    }

    // Copy the read data to a buffer for verification
    lcopy(SD_SECTORBUFFER, (Addr28)sd_verify_buffer, SD_SECTOR_SIZE);

    uint16_t i;
    for (i = 0; i < SD_SECTOR_SIZE; i++) {
        if (sector_buffer[i] != sd_verify_buffer[i]) {
            break;
        }
    }
    if (i == SD_SECTOR_SIZE) {
        DEBUG_COUNT(sd_writes_skipped);
        return true;
    }

    while (tries < 10) {
        DEBUG_COUNT(sd_writes);

        // Copy data to hardware sector buffer via DMA
        lcopy((Addr28)sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);

        if (!wait_before_write(is_multi)) {
            return false;
        }

        SD_COMMAND = SD_CMD_WRITE_GATE;
        if (is_multi) {
            SD_COMMAND = SD_CMD_WRITE_MULTI_FIRST;
        } else {
            SD_COMMAND = SD_CMD_WRITE;
        }

        // Wait for the write to complete
        if (!wait_before_write(is_multi)) {
            return false;
        }

        if (!(SD_STATUS & SD_STATUS_UNSETTLED)) {
            /* The controller misbehaves unless a read follows a write, and
             * sometimes wants a reset even then; the read below is also what
             * verifies the write took. */
            DEBUG_COUNT(sd_reads);
            SD_COMMAND = SD_CMD_READ;

            while (SD_STATUS & SD_STATUS_BUSY) {
            }

            // Copy the read data to a buffer for verification
            lcopy(SD_SECTORBUFFER, (Addr28)sd_verify_buffer, SD_SECTOR_SIZE);

            for (i = 0; i < SD_SECTOR_SIZE; i++) {
                if (sector_buffer[i] != sd_verify_buffer[i]) {
                    break;
                }
            }
            if (i == SD_SECTOR_SIZE) {
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
    for (uint8_t round = 0; round < SD_READY_ROUNDS; round++) {
        uint16_t spins = 0;
        do {
            if (!(SD_STATUS & SD_STATUS_BUSY)) {
                return true;
            }
        } while (++spins);
    }
    return false;
}

/* Opens a multi-block write and sends its first block.
 *
 * Separate from sdcard_writesector() because that one cannot serve: it reads
 * the sector back to verify, and sdcardio.vhdl shows what any other traffic
 * does to an open stream -- command $03 clears sd_write_multi outright, and a
 * read leaves the card part way through a CMD25 it will never be told to
 * finish.  Measured on hardware: the first block landed and every one after it
 * was lost.  It also short-circuits when the sector already holds the wanted
 * bytes, which would leave the stream unopened and the blocks after it
 * addressed at nothing.
 *
 * So this verifies nothing.  The batch is read back once the stream is closed,
 * which is what mega65-tools' remotesd_eth.c does. */
bool sdcard_writefirstsector(const uint32_t sector_number) {
    const uint32_t sector_address = sdhc_card ? sector_number : sector_number * SD_SECTOR_SIZE;
    SD_COMMAND = SD_CMD_RESET_END;
    SD_SECTOR_ADDR(0) = (sector_address >> 0) & 0xff;
    SD_SECTOR_ADDR(1) = (sector_address >> 8) & 0xff;
    SD_SECTOR_ADDR(2) = (sector_address >> 16) & 0xff;
    SD_SECTOR_ADDR(3) = (sector_address >> 24) & 0xff;

    if (!sdcard_ready()) {
        return false;
    }
    lcopy((Addr28)sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);
    SD_COMMAND = SD_CMD_WRITE_GATE;
    SD_COMMAND = SD_CMD_WRITE_MULTI_FIRST;
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
    lcopy((Addr28)sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);
    SD_COMMAND = SD_CMD_WRITE_GATE;
    SD_COMMAND = SD_CMD_WRITE_MULTI_NEXT;
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
    lcopy((Addr28)sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);
    SD_COMMAND = SD_CMD_WRITE_GATE;
    SD_COMMAND = SD_CMD_WRITE_MULTI_LAST;
    return sdcard_ready();
}
