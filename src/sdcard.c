#include "sdcard.h"

#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"

#include <mega65.h>
#include <stdio.h>
#include <stdlib.h>

const long SD_SECTORBUFFER = 0xffd6e00L;

unsigned char sdhc_card = 0;
uint8_t hal_border_flicker = 0;

/* Whether the border cycles while the controller is busy: 0 silent, above 1
 * reports.  1 is accepted and does nothing -- no site tests for it -- so a
 * caller that wants to be told anything asks for 2. */
void sdcard_visual_feedback(const uint8_t do_flicker) {
    hal_border_flicker = do_flicker < 3 ? do_flicker : 2;
}

void sdcard_reset(void) {
    SD_COMMAND = 0;
    SD_COMMAND = 1;

    // Now wait for SD card reset to complete
    while (SD_STATUS & SD_STATUS_BUSY) {
        if (hal_border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
    }

    if (sdhc_card) {
        /* Writes do not take without this, though nothing says why. */
        SD_COMMAND = 0x41;
    }
}

void mega65_fast(void) {
    // Fast CPU
    POKE(0, 65);
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
    char tries = 0;
    uint16_t timeout;

    uint32_t sector_address = sector_number * 512;
    if (sdhc_card) {
        sector_address = sector_number;
    } else {
        if (sector_number >= 0x7fffff) {
            while (1) {
            }
        }
    }

    SD_SECTOR(0) = (sector_address >> 0) & 0xff;
    SD_SECTOR(1) = (sector_address >> 8) & 0xff;
    SD_SECTOR(2) = ((uint32_t)sector_address >> 16) & 0xff;
    SD_SECTOR(3) = ((uint32_t)sector_address >> 24) & 0xff;

    while (tries < 10) {
        DEBUG_COUNT(sd_reads);

        // Wait for SD card to be ready
        timeout = 50000U;
        while (SD_STATUS & SD_STATUS_BUSY) {
            timeout--;
            if (!timeout) {
                // Time out -- so reset SD card
                SD_COMMAND = 0;
                SD_COMMAND = 1;
                timeout = 50000U;
            }
            if (SD_STATUS & 0x40) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (SD_STATUS == 0x01) {
                return;
            }
        }

        // Command read
        SD_COMMAND = 2;

        // Wait for read to complete
        timeout = 50000U;
        while (SD_STATUS & SD_STATUS_BUSY) {
            timeout--;
            if (!timeout) {
                return;
            }
            if (SD_STATUS & 0x40) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (SD_STATUS == 0x01) {
                return;
            }
        }

        if (!(SD_STATUS & 0x67)) {
            // Copy data from hardware sector buffer via DMA
            lcopy(SD_SECTORBUFFER, (long)sector_buffer, 512);

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

uint8_t verify_buffer[512];

void sdcard_writesector(const uint32_t sector_number, uint8_t is_multi) {
    // Copy buffer into the SD card buffer, and then execute the write job
    uint32_t sector_address;
    int i;
    char tries = 0;
    uint16_t counter = 0;

    // Set address to read/write
    SD_COMMAND = 1; // end reset
    if (!sdhc_card) {
        sector_address = sector_number * 512;
    } else {
        sector_address = sector_number;
    }
    SD_SECTOR(0) = (sector_address >> 0) & 0xff;
    SD_SECTOR(1) = (sector_address >> 8) & 0xff;
    SD_SECTOR(2) = (sector_address >> 16) & 0xff;
    SD_SECTOR(3) = (sector_address >> 24) & 0xff;

    // Read the sector and see if it already has the correct contents.
    // If so, nothing to write

    DEBUG_COUNT(sd_reads);
    SD_COMMAND = 2; // read the sector we just wrote

    counter = 0;
    while (SD_STATUS & SD_STATUS_BUSY) {
        if (hal_border_flicker > 1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
        }
        counter++;
        if (!counter) {
            // SD card not becoming ready: try reset
            SD_COMMAND = 0; // begin reset
            usleep(500000);
            SD_COMMAND = 1; // end reset
            SD_COMMAND = 2;
        }
    }

    // Copy the read data to a buffer for verification
    lcopy(SD_SECTORBUFFER, (long)verify_buffer, 512);

    for (i = 0; i < 512; i++) {
        if (sector_buffer[i] != verify_buffer[i]) {
            break;
        }
    }
    if (i == 512) {
        DEBUG_COUNT(sd_writes_skipped);
        return;
    }

    while (tries < 10) {
        DEBUG_COUNT(sd_writes);

        // Copy data to hardware sector buffer via DMA
        lcopy((long)sector_buffer, SD_SECTORBUFFER, 512);

        // Wait for SD card to be ready
        counter = 0;
        while (SD_STATUS & SD_STATUS_BUSY) {
            counter++;
            if (!counter) {
                // SD card not becoming ready: try reset
                SD_COMMAND = 0; // begin reset
                usleep(500000);
                SD_COMMAND = 1;    // end reset
                SD_COMMAND = 0x57; // Open SD card write gate
                if (is_multi) {
                    SD_COMMAND = 4;
                } else {
                    SD_COMMAND = 3; // retry write
                }
            }
        }

        // Command write
        SD_COMMAND = 0x57; // Open SD card write gate
        if (is_multi) {
            SD_COMMAND = 4;
        } else {
            SD_COMMAND = 3;
        }

        // Wait for write to complete
        counter = 0;
        while (SD_STATUS & SD_STATUS_BUSY) {
            counter++;
            if (!counter) {
                // SD card not becoming ready: try reset
                SD_COMMAND = 0; // begin reset
                usleep(500000);
                SD_COMMAND = 1; // end reset
                // Retry write
                SD_COMMAND = 0x57; // Open SD card write gate
                if (is_multi) {
                    SD_COMMAND = 4;
                } else {
                    SD_COMMAND = 3;
                }
            }
        }

        if (!(SD_STATUS & 0x67)) {
            /* The controller misbehaves unless a read follows a write, and
             * sometimes wants a reset even then; the read below is also what
             * verifies the write took. */
            DEBUG_COUNT(sd_reads);
            SD_COMMAND = 2;

            while (SD_STATUS & SD_STATUS_BUSY) {
            }

            // Copy the read data to a buffer for verification
            lcopy(SD_SECTORBUFFER, (long)verify_buffer, 512);

            for (i = 0; i < 512; i++) {
                if (sector_buffer[i] != verify_buffer[i]) {
                    break;
                }
            }
            if (i != 512) {
                screen_hex(screen_line_address - 80 + 24, sector_number);
            } else {
                return;
            }
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
    lcopy((long)sector_buffer, SD_SECTORBUFFER, 512);

    // Command write of follow-on block in multi-block write job
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
    SD_COMMAND = 0x57; // Open SD card write gate
    SD_COMMAND = 5;
    while (!(SD_STATUS & SD_STATUS_BUSY)) {
    }
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
}

void sdcard_writemultidone(void) {
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
    SD_COMMAND = 0x57;
    SD_COMMAND = 6;
    while (!(SD_STATUS & SD_STATUS_BUSY)) {
    }
    while (SD_STATUS & SD_STATUS_BUSY) {
    }
}
