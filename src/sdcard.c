#include "sdcard.h"

#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"

#include <mega65.h>
#include <stdio.h>
#include <stdlib.h>

constexpr Addr28 SD_SECTORBUFFER = 0xffd6e00;

unsigned char sdhc_card = 0;
uint8_t hal_border_flicker = 0;

/* Half a second for the card to come back after a reset. */
constexpr uint32_t SD_RESET_SETTLE_MICROSECS = 500000;
/* Polls, not time: how long a read waits before the card counts as stalled.
 * The controller answers in microseconds when it is well. */
constexpr uint16_t SD_READY_POLLS = 50000;
/* A card that is not SDHC is addressed by byte, and sector * SD_SECTOR_SIZE has to stay
 * inside 32 bits, so the sector number stops here. */
constexpr uint32_t SD_MAX_SECTOR_BYTE_ADDRESSED = 0x7FFFFF;

/* Whether the border cycles while the controller is busy: 0 silent, above 1
 * reports.  1 is accepted and does nothing -- no site tests for it -- so a
 * caller that wants to be told anything asks for 2. */
void sdcard_visual_feedback(const uint8_t do_flicker) {
    hal_border_flicker = do_flicker < 3 ? do_flicker : 2;
}

void sdcard_reset(void) {
    SD_COMMAND = SD_CMD_RESET_BEGIN;
    SD_COMMAND = SD_CMD_RESET_END;

    // Now wait for SD card reset to complete
    while (SD_STATUS & SD_STATUS_BUSY) {
        if (hal_border_flicker > 1) {
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

        if (hal_border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }

        // Reset SD card
        sdcard_open();

        tries++;
    }
}

/* Placed by src/memory.ld, above the region the linker allocates from, so its
 * half kilobyte does not come out of the same budget as the code. */
extern uint8_t sd_verify_buffer[SD_SECTOR_SIZE];

void sdcard_writesector(const uint32_t sector_number, uint8_t is_multi) {
    uint8_t tries = 0;
    const uint32_t sector_address = sdhc_card ? sector_number : sector_number * SD_SECTOR_SIZE;

    // Set address to read/write
    SD_COMMAND = SD_CMD_RESET_END;
    SD_SECTOR_ADDR(0) = (sector_address >> 0) & 0xff;
    SD_SECTOR_ADDR(1) = (sector_address >> 8) & 0xff;
    SD_SECTOR_ADDR(2) = (sector_address >> 16) & 0xff;
    SD_SECTOR_ADDR(3) = (sector_address >> 24) & 0xff;

    // Read the sector and see if it already has the correct contents.
    // If so, nothing to write

    DEBUG_COUNT(sd_reads);
    SD_COMMAND = SD_CMD_READ;

    /* The count wrapping is the timeout: 65536 polls without the card coming
     * ready, and it is reset and asked again. */
    uint16_t counter = 0;
    while (SD_STATUS & SD_STATUS_BUSY) {
        if (hal_border_flicker > 1) {
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
        return;
    }

    while (tries < 10) {
        DEBUG_COUNT(sd_writes);

        // Copy data to hardware sector buffer via DMA
        lcopy((Addr28)sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);

        // Wait for SD card to be ready
        uint16_t counter = 0;
        while (SD_STATUS & SD_STATUS_BUSY) {
            if (!++counter) {
                // SD card not becoming ready: try reset
                SD_COMMAND = SD_CMD_RESET_BEGIN;
                usleep(SD_RESET_SETTLE_MICROSECS);
                SD_COMMAND = SD_CMD_RESET_END;
                SD_COMMAND = SD_CMD_WRITE_GATE;
                if (is_multi) {
                    SD_COMMAND = SD_CMD_WRITE_MULTI_FIRST;
                } else {
                    SD_COMMAND = SD_CMD_WRITE;
                }
            }
        }

        SD_COMMAND = SD_CMD_WRITE_GATE;
        if (is_multi) {
            SD_COMMAND = SD_CMD_WRITE_MULTI_FIRST;
        } else {
            SD_COMMAND = SD_CMD_WRITE;
        }

        // Wait for write to complete
        counter = 0;
        while (SD_STATUS & SD_STATUS_BUSY) {
            if (!++counter) {
                // SD card not becoming ready: try reset
                SD_COMMAND = SD_CMD_RESET_BEGIN;
                usleep(SD_RESET_SETTLE_MICROSECS);
                SD_COMMAND = SD_CMD_RESET_END;
                // Retry write
                SD_COMMAND = SD_CMD_WRITE_GATE;
                if (is_multi) {
                    SD_COMMAND = SD_CMD_WRITE_MULTI_FIRST;
                } else {
                    SD_COMMAND = SD_CMD_WRITE;
                }
            }
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
                return;
            }
            screen_hex(screen_line_address - 80 + 24, sector_number);
        }

        if (hal_border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
        /* Without this the bound is never approached and a sector that will not
         * verify is rewritten for ever. */
        tries++;
    }
}

void sdcard_writenextsector(void) {
    // Copy data to hardware sector buffer via DMA
    lcopy((Addr28)sector_buffer, SD_SECTORBUFFER, SD_SECTOR_SIZE);

    // Command write of follow-on block in multi-block write job
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
    SD_COMMAND = SD_CMD_WRITE_GATE;
    SD_COMMAND = SD_CMD_WRITE_MULTI_NEXT;
    while (!(SD_STATUS & SD_STATUS_BUSY)) {
    }
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
}

void sdcard_writemultidone(void) {
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
    SD_COMMAND = SD_CMD_WRITE_GATE;
    SD_COMMAND = SD_CMD_WRITE_MULTI_LAST;
    while (!(SD_STATUS & SD_STATUS_BUSY)) {
    }
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
}
