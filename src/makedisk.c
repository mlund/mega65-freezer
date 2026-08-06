/*
  Based on mega65-fdisk program as a starting point.

*/

// D81 geometry, unrelated to screen geometry despite sharing the number 80.
#define D81_TRACKS 80
#define D81_SECTORS_PER_TRACK 20

#include "fdisk_fat32.h"
#include "fdisk_hal.h"
#include "fdisk_memory.h"
#include "fdisk_screen.h"
#include "freezer.h"
#include "freezer_common.h"
#include "mega65_regs.h"

#include <mega65.h>
#include <stdio.h>
#include <string.h>

void setup_menu_screen(void) {
    setup_menu_screen_base();

    VICIV.ctrlc = (VICIV.ctrlc & VIC4_CTRLC_MODE_MASK) | VIC4_CTRLC_16BIT_FULL_COLOUR;
    VICIV.linestep = SCREEN_ROW_BYTES;
    /* Colour RAM deliberately left alone here. */
}

void draw_box(unsigned char x1,
    unsigned char y1,
    unsigned char x2,
    unsigned char y2,
    unsigned char colour,
    unsigned char erase) {
    unsigned char x, y;

    // Clear colour RAM
    for (x = x1; x <= x2; x++) {
        for (y = y1; y <= y2; y++) {
            lpoke(COLOUR_RAM_ADDRESS + y * SCREEN_ROW_BYTES + x * 2 + 1, colour);
            lpoke(COLOUR_RAM_ADDRESS + y * SCREEN_ROW_BYTES + x * 2 + 0, 0);
        }
    }

    if (erase) {
        for (x = x1 + 1; x < x2; x++) {
            for (y = y1 + 1; y < y2; y++) {
                lpoke(SCREEN_ADDRESS + y * SCREEN_ROW_BYTES + x * 2 + 0, 0x20);
                lpoke(SCREEN_ADDRESS + y * SCREEN_ROW_BYTES + x * 2 + 1, 0);
            }
        }
    }

    for (x = x1; x < x2; x++) {
        lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x * 2, 0x40); // horizontal line, centred
        lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x * 2 + 1, 0);
        lpoke(SCREEN_ADDRESS + y2 * SCREEN_ROW_BYTES + x * 2, 0x40); // horizontal line, centred
        lpoke(SCREEN_ADDRESS + y2 * SCREEN_ROW_BYTES + x * 2 + 1, 0);
    }

    for (y = y1; y < y2; y++) {
        lpoke(SCREEN_ADDRESS + y * SCREEN_ROW_BYTES + x1 * 2, 0x42); // vertical line, centred
        lpoke(SCREEN_ADDRESS + y * SCREEN_ROW_BYTES + x1 * 2 + 1, 0);
        lpoke(SCREEN_ADDRESS + y * SCREEN_ROW_BYTES + x2 * 2, 0x42); // vertical line, centred
        lpoke(SCREEN_ADDRESS + y * SCREEN_ROW_BYTES + x2 * 2 + 1, 0);
    }
    lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x1 * 2, 0x55); // top left corner
    lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x2 * 2, 73);   // top right corner
    lpoke(SCREEN_ADDRESS + y2 * SCREEN_ROW_BYTES + x1 * 2, 74);   // bottom left corner
    lpoke(SCREEN_ADDRESS + y2 * SCREEN_ROW_BYTES + x2 * 2, 75);   // bottom right corner
    lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x1 * 2 + 1, 0);
    lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x2 * 2 + 1, 0);
    lpoke(SCREEN_ADDRESS + y2 * SCREEN_ROW_BYTES + x1 * 2 + 1, 0);
    lpoke(SCREEN_ADDRESS + y2 * SCREEN_ROW_BYTES + x2 * 2 + 1, 0);
}

void write_text(unsigned char x1, unsigned char y1, unsigned char colour, const char* t) {
    unsigned char x, c;
    for (x = x1; t[x - x1]; x++) {
        c = t[x - x1];
        if (c > 0x60) {
            c -= 0x60;
        }
        if (c > 0x40) {
            c -= 0x40;
        }
        lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x * 2 + 0, c);
        lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x * 2 + 1, 0);
        lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + x * 2 + 0, 0x00);
        lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + x * 2 + 1, colour);
    }
}

void input_text(
    unsigned char x1, unsigned char y1, unsigned char len, unsigned char colour, char* out) {
    unsigned char ofs = 0, x, c;
    for (x = x1; x < (x1 + len); x++) {
        lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x1 * 2 + 0, ' ');
        lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + x1 * 2 + 1, 0);
        lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + x1 * 2 + 0, 0x00);
        lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + x1 * 2 + 1, colour);
    }

    out[0] = 0;

    while (1) {
        // Enable cursor on current char
        lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + (x1 + ofs) * 2 + 1, colour | 0x30);

        c = ASCIIKEY;
        if ((c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39)) {
            if (ofs < len) {
                out[ofs] = c;
                // ASCII to screen code conversion
                if (c > 0x60) {
                    c -= 0x60;
                }
                lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + (x1 + ofs) * 2 + 0, c);
                lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + (x1 + ofs) * 2 + 1, colour);
                ofs++;
            }
        } else {
            switch (c) {
                case 0x14: // delete
                    // XXX actually copy chars down, instead of just erasing from
                    // end of line, and allow cursor left and right
                    lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + (x1 + ofs) * 2 + 0, ' ');
                    lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + (x1 + ofs) * 2 + 1, colour);
                    if (ofs) {
                        ofs--;
                    }
                    lpoke(SCREEN_ADDRESS + y1 * SCREEN_ROW_BYTES + (x1 + ofs) * 2 + 0, ' ');
                    lpoke(COLOUR_RAM_ADDRESS + y1 * SCREEN_ROW_BYTES + (x1 + ofs) * 2 + 1, colour);
                    break;
                case 0x03:
                    out[0] = 0;
                    ASCIIKEY = 0;
                    return;
                case 0x0d:
                    out[ofs] = 0;
                    ASCIIKEY = 0;
                    return;
                default:
                    break;
            }
        }
        if (c) {
            ASCIIKEY = 0;
        }
    }
}

// clang-format off
unsigned char bam_sector1[0x100] = {
  0x28, 0x02, 0x44, 0xbb, 0x39, 0x38, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff,
  0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff,
  0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff,
  0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff,
  0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff,
  0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff,
  0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff,
  0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff,
  0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff,
  0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x28, 0xff,
  0xff, 0xff, 0xff, 0xff, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0x24, 0xf0, 0xff, 0xff, 0xff, 0xff
};
// clang-format on

unsigned char to_hex(unsigned char i) {
    if (i < 10) {
        return '0' + i;
    }
    return 0x41 + i - 10;
}

void format_disk_image(unsigned long file_sector, char* diskname, unsigned char is_d65) {
    unsigned char i;
    unsigned short s;
    unsigned short sect_count = D81_TRACKS * D81_SECTORS_PER_TRACK;
    if (is_d65) {
        sect_count = 85 * 64;
    }

    // Make sure entire image is empty
    clear_sector_buffer();
    for (s = 0; s < sect_count; s++) {
        // XXX - Using multi-sector writes here would be much faster
        sdcard_writesector(file_sector + s, 0);
    }

    // Link to first directory sector
    sector_buffer[0] = 0x28;
    sector_buffer[1] = 0x03;

    // DOS Version
    sector_buffer[2] = 0x44;
    sector_buffer[3] = 0x00;

    // Diskname
    lcopy((long)diskname, (long)&sector_buffer[4], 16);
    if (strlen(diskname) < 16) {
        for (i = (unsigned char)strlen(diskname); i < 16; i++) {
            sector_buffer[4 + i] = 0xa0;
        }
    }

    sector_buffer[0x14] = 0xa0;
    sector_buffer[0x15] = 0xa0;

    // Random disk ID
    i = VICIV.rasterline;
    sector_buffer[0x16] = to_hex(i & 0xf);
    sector_buffer[0x17] = to_hex(i >> 4);

    sector_buffer[0x18] = 0xa0;

    // DOS type
    sector_buffer[0x19] = 0x31;
    sector_buffer[0x1A] = 0x44;

    sector_buffer[0x1B] = 0xa0;
    sector_buffer[0x1C] = 0xa0;

    lcopy((long)bam_sector1, (long)&sector_buffer[0x100], 0x100);

    // Disk ID in BAM
    sector_buffer[0x104] = to_hex(i & 0xf);
    sector_buffer[0x105] = to_hex(i >> 4);

    if (!is_d65) {
        sdcard_writesector(file_sector + (39 * 10 * 2 + 0), 0);
    } else {
        sdcard_writesector(file_sector + (39 * 64 * 2 + 0), 0);
    }

    clear_sector_buffer();
    lcopy((long)bam_sector1, (long)sector_buffer, 0x100);
    // Disk ID in BAM
    sector_buffer[0x004] = to_hex(i & 0xf);
    sector_buffer[0x005] = to_hex(i >> 4);
    sector_buffer[0x101] = 0xff;
    // Link to first sector of dir
    sector_buffer[0x000] = 0x00;
    sector_buffer[0x001] = 0xFF;
    // Mark all sectors free in 2nd half of disk
    sector_buffer[0x0FA] = 40;
    sector_buffer[0x0FB] = 0xff;

    if (!is_d65) {
        sdcard_writesector(file_sector + (39 * 10 * 2 + 1), 0);
    } else {
        sdcard_writesector(file_sector + (39 * 64 * 2 + 1), 0);
    }
}

void do_make_disk_image(unsigned char is_d65, unsigned char drive_id) {
    char diskname[16 + 1];
    char filename[16 + 1];
    unsigned char filename_len;
    unsigned long file_sector;

    fat32_open_file_system();
    if (!fat1_sector) {
        draw_box(10, 8, 30, 13, 2, 1);
        write_text(11, 9, 7, "COULD NOT FIND SD CARD");
        while (!ASCIIKEY) {
        }
        ASCIIKEY = 0;
        return;
    }

    draw_box(10, 8, 30, 14, 14, 1);
    write_text(11, 9, 14, "ENTER NAME FOR");
    if (is_d65) {
        write_text(11, 10, 14, "HD (D65) IMAGE:");
    } else {
        write_text(11, 10, 14, "DD (D81) IMAGE:");
    }
    input_text(11, 12, 8, 1, filename);
    for (filename_len = 0; filename[filename_len]; filename_len++) {
        // Convert to upper case and work out length of string
        if (filename[filename_len] >= 0x61 && filename[filename_len] <= 0x7a) {
            filename[filename_len] -= 0x20;
        }
    }
    if (!filename_len) {
        return;
    }

    // Copy filename into diskname before it gets extended by the filename extension
    strcpy(diskname, filename);

    filename[filename_len++] = '.';
    filename[filename_len++] = 0x44;
    if (is_d65) {
        filename[filename_len++] = 0x36;
        filename[filename_len++] = 0x35;
    } else {
        filename[filename_len++] = 0x38;
        filename[filename_len++] = 0x31;
    }
    filename[filename_len] = 0;
    lcopy((long)filename, 0x0400, 16);

    draw_box(10, 8, 30, 14, 7, 1);
    write_text(11, 9, 7, "CREATING IMAGE...");

    // Actually create the file
    //  while(!ASCIIKEY) VICIV.bordercol = VICIV.bordercol+1; ASCIIKEY = 0;
    file_sector = fat32_create_contiguous_file(filename,
        is_d65 ? (85 * 64 * 2 * 512L) : (D81_TRACKS * 10L * 2 * SECTOR_SIZE),
        root_dir_sector,
        fat1_sector,
        fat2_sector);
    if (!file_sector) {
        // Error making file
        draw_box(10, 8, 30, 14, 2, 1);
        write_text(11, 9, 2, "Error creating file");
        write_text(11, 12, 1, "Press almost any key...");
        while (!ASCIIKEY) {
        }
        ASCIIKEY = 0;
    } else {
        // File creation succeeded

        // Write header, BAM and zero out directory track
        write_text(11, 10, 14, "FORMATTING IMAGE...");
        format_disk_image(file_sector, diskname, is_d65);

        draw_box(8, 8, 32, 14, 13, 1);
        write_text(9, 9, 13, "Created disk image");

        // now mount the new image on the drive_id we got
        mega65_dos_attach(filename, drive_id);
        // store mount to freeze slot
        copy_imageproc_to_freezeregion(drive_id, 0);

        write_text(9, 12, 1, "Press almost any key...");

        while (!ASCIIKEY) {
        }
        ASCIIKEY = 0;
    }
}

int main(void) {
    /* Performs the $D02F knock; without it every later write to a VIC-IV
     * register such as $D054 is silently ignored and the screen mode is
     * never established. */
    mega65_fast();

    // Disable interrupts and interrupt sources
    __asm__ volatile("sei" ::: "memory");
    CIA1.icr = 0x7F;
    CIA2.icr = 0x7F;
    VICIV.imr = 0x00;
    // XXX add missing C65 AND M65 peripherals
    // C65 UART, ethernet etc

    // Bank out BASIC ROM, leave KERNAL and IO in
    POKE(0x00, 0x3F);
    POKE(0x01, 0x36);

    // No decimal mode!
    __asm__ volatile("cld");

    // Enable extended attributes so we can use reverse
    VICIV.ctrlb = VICIV.ctrlb | 0x20;

    // Correct horizontal scaling
    VICIV.chrxscl = 0x78;

    // Silence SIDs
    SID1.amp = 0;
    SID2.amp = 0;

    set_palette();

    // Now find the start sector of the slot, and make a copy for safe keeping
    slot_number = 0;
    freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);

    // SD or SDHC card?
    if (SD_STATUS & SD_STATUS_SDHC) {
        sdhc_card = 1;
    } else {
        sdhc_card = 0;
    }

    setup_menu_screen();

    request_freeze_region_list();

    do_make_disk_image(PEEK(0x33C) ? 1 : 0, PEEK(0x3C0) ? 1 : 0); // 0=DD, 1=HD

    mega65_dos_exechelper("FREEZER.M65");
}
