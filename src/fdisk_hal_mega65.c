#include "fdisk_hal.h"
#include "fdisk_memory.h"
#include "fdisk_screen.h"
#include "mega65_regs.h"

#include <mega65.h>
#include <stdio.h>
#include <stdlib.h>

const long SD_SECTORBUFFER = 0xffd6e00L;
const uint16_t SD_CTL = 0xd680L;
const uint16_t SD_ADDR = 0xd681L;

unsigned char sdhc_card = 0;
uint8_t hal_border_flicker = 0;

/*
 * sdcard_visual_feedback(do_flicker)
 *
 *   do_flicker 0 == no border change
 *              1 == only success/fail display
 *              2 == full progress
 */
void sdcard_visual_feedback(const uint8_t do_flicker) {
    hal_border_flicker = do_flicker < 3 ? do_flicker : 2;
}

void sdcard_reset(void) {
    // Reset and release reset
    //  write_line("Resetting SD card...",0);

    POKE(SD_CTL, 0);
    POKE(SD_CTL, 1);

    // Now wait for SD card reset to complete
    while (PEEK(SD_CTL) & 3)
        if (hal_border_flicker > 1)
            VICIV.bordercol = (VICIV.bordercol + 1) & 0xf;

    if (sdhc_card) {
        // Set SDHC flag (else writing doesnt work for some reason)
        //    write_line("Setting SDHC mode",0);
        POKE(SD_CTL, 0x41);
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

uint32_t write_count = 0;

void sdcard_readsector(const uint32_t sector_number) {
    char tries = 0;
    unsigned short timeout;

    uint32_t sector_address = sector_number * 512;
    if (sdhc_card)
        sector_address = sector_number;
    else {
        if (sector_number >= 0x7fffff) {
            //      write_line("ERROR: Asking for sector @ >= 4GB on SDSC card.",0);
            while (1)
                continue;
        }
    }

    POKE(SD_ADDR + 0, (sector_address >> 0) & 0xff);
    POKE(SD_ADDR + 1, (sector_address >> 8) & 0xff);
    POKE(SD_ADDR + 2, ((uint32_t)sector_address >> 16) & 0xff);
    POKE(SD_ADDR + 3, ((uint32_t)sector_address >> 24) & 0xff);

    // write_line("Reading sector @ $",0);
    //  screen_hex(screen_line_address-80+18,sector_address);

    while (tries < 10) {

        // Wait for SD card to be ready
        timeout = 50000U;
        while (PEEK(SD_CTL) & 0x3) {
            timeout--;
            if (!timeout) {
                // Time out -- so reset SD card
                POKE(SD_CTL, 0);
                POKE(SD_CTL, 1);
                timeout = 50000U;
            }
            if (PEEK(SD_CTL) & 0x40) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (PEEK(SD_CTL) == 0x01)
                return;
        }

        // Command read
        POKE(SD_CTL, 2);

        // Wait for read to complete
        timeout = 50000U;
        while (PEEK(SD_CTL) & 0x3) {
            timeout--;
            if (!timeout)
                return;
            //      write_line("Waiting for read to complete",0);
            if (PEEK(SD_CTL) & 0x40) {
                return;
            }
            // Sometimes we see this result, i.e., sdcard.vhdl thinks it is done,
            // but sdcardio.vhdl thinks not. This means a read error
            if (PEEK(SD_CTL) == 0x01)
                return;
        }

        // Note result
        // result=PEEK(sd_ctl);

        if (!(PEEK(SD_CTL) & 0x67)) {
            // Copy data from hardware sector buffer via DMA
            lcopy(SD_SECTORBUFFER, (long)sector_buffer, 512);

            return;
        }

        if (hal_border_flicker > 1)
            VICIV.bordercol = (VICIV.bordercol + 1) & 0xf;

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
    POKE(SD_CTL, 1); // end reset
    if (!sdhc_card)
        sector_address = sector_number * 512;
    else
        sector_address = sector_number;
    POKE(SD_ADDR + 0, (sector_address >> 0) & 0xff);
    POKE(SD_ADDR + 1, (sector_address >> 8) & 0xff);
    POKE(SD_ADDR + 2, (sector_address >> 16) & 0xff);
    POKE(SD_ADDR + 3, (sector_address >> 24) & 0xff);

    // Read the sector and see if it already has the correct contents.
    // If so, nothing to write

    POKE(SD_CTL, 2); // read the sector we just wrote

    counter = 0;
    while (PEEK(SD_CTL) & 3) {
        if (hal_border_flicker > 1)
            VICIV.bordercol = (VICIV.bordercol + 1) & 0xf;
        counter++;
        if (!counter) {
            // SD card not becoming ready: try reset
            POKE(SD_CTL, 0); // begin reset
            usleep(500000);
            POKE(SD_CTL, 1); // end reset
            POKE(SD_CTL, 2);
        }
    }

    // Copy the read data to a buffer for verification
    lcopy(SD_SECTORBUFFER, (long)verify_buffer, 512);

    // VErify that it matches the data we wrote
    for (i = 0; i < 512; i++) {
        if (sector_buffer[i] != verify_buffer[i])
            break;
    }
    if (i == 512) {
        return;
    }

    while (tries < 10) {

        // Copy data to hardware sector buffer via DMA
        lcopy((long)sector_buffer, SD_SECTORBUFFER, 512);

        // Wait for SD card to be ready
        counter = 0;
        while (PEEK(SD_CTL) & 3) {
            counter++;
            if (!counter) {
                // SD card not becoming ready: try reset
                POKE(SD_CTL, 0); // begin reset
                usleep(500000);
                POKE(SD_CTL, 1);    // end reset
                POKE(SD_CTL, 0x57); // Open SD card write gate
                if (is_multi)
                    POKE(SD_CTL, 4);
                else
                    POKE(SD_CTL, 3); // retry write
            }
            // Show we are doing something
            //	POKE(0x804f,1+(PEEK(0x804f)&0x7f));
        }

        // Command write
        POKE(SD_CTL, 0x57); // Open SD card write gate
        if (is_multi)
            POKE(SD_CTL, 4);
        else
            POKE(SD_CTL, 3);

        // Wait for write to complete
        counter = 0;
        while (PEEK(SD_CTL) & 3) {
            counter++;
            if (!counter) {
                // SD card not becoming ready: try reset
                POKE(SD_CTL, 0); // begin reset
                usleep(500000);
                POKE(SD_CTL, 1); // end reset
                // Retry write
                POKE(SD_CTL, 0x57); // Open SD card write gate
                if (is_multi)
                    POKE(SD_CTL, 4);
                else
                    POKE(SD_CTL, 3);
            }
            // Show we are doing something
            //	POKE(0x809f,1+(PEEK(0x809f)&0x7f));
        }

        write_count++;
        if (hal_border_flicker > 1)
            VICIV.bordercol = write_count & 0x0f;

        if (!(PEEK(SD_CTL) & 0x67)) {
            write_count++;

            if (hal_border_flicker > 1)
                VICIV.bordercol = write_count & 0x0f;

            // There is a bug in the SD controller: You have to read between writes, or it
            // gets really upset.

            // But sometimes even that doesn't work, and we have to reset it.

            // Does it just need some time between accesses?

            POKE(SD_CTL, 2); // read the sector we just wrote

            while (PEEK(SD_CTL) & 3) {
                continue;
            }

            // Copy the read data to a buffer for verification
            lcopy(SD_SECTORBUFFER, (long)verify_buffer, 512);

            // VErify that it matches the data we wrote
            for (i = 0; i < 512; i++) {
                if (sector_buffer[i] != verify_buffer[i])
                    break;
            }
            if (i != 512) {
                // VErify error has occurred
                //	write_line("Verify error for sector $$$$$$$$",0);
                screen_hex(screen_line_address - 80 + 24, sector_number);
            } else {
                //      write_line("Wrote sector $$$$$$$$, result=$$",2);
                //      screen_hex(screen_line_address-80+2+14,sector_number);
                //      screen_hex(screen_line_address-80+2+30,result);

                return;
            }
        }

        if (hal_border_flicker > 1)
            VICIV.bordercol = (VICIV.bordercol + 1) & 0xf;
    }

    //  write_line("Write error @ $$$$$$$$$",2);
    //  screen_hex(screen_line_address-80+2+16,sector_number);
}

void sdcard_writenextsector(void) {
    // Copy data to hardware sector buffer via DMA
    lcopy((long)sector_buffer, SD_SECTORBUFFER, 512);

    // Command write of follow-on block in multi-block write job
    while (PEEK(SD_CTL) & 3) {
        continue;
    }
    POKE(SD_CTL, 0x57); // Open SD card write gate
    POKE(SD_CTL, 5);
    while (!(PEEK(SD_CTL) & 3)) {
        continue;
    }
    while (PEEK(SD_CTL) & 3) {
        continue;
    }
}

void sdcard_writemultidone(void) {
    while (PEEK(SD_CTL) & 3) {
        continue;
    }
    POKE(SD_CTL, 0x57);
    POKE(SD_CTL, 6);
    while (!(PEEK(SD_CTL) & 3)) {
        continue;
    }
    while (PEEK(SD_CTL) & 3) {
        continue;
    }
}
