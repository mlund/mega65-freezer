/*
  Based on mega65-fdisk program as a starting point.

*/

#include "colours.h"
#include "common.h"
#include "dma.h"
#include "fat32.h"
#include "layout.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

#include <mega65.h>
#include <stdio.h>
#include <string.h>

static void draw_box(unsigned char left,
    unsigned char top,
    unsigned char right,
    unsigned char bottom,
    unsigned char colour,
    unsigned char erase) {
    unsigned char column;
    unsigned char row;

    // Clear colour RAM
    for (column = left; column <= right; column++) {
        for (row = top; row <= bottom; row++) {
            colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + column * 2 + 1), colour);
            colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + column * 2 + 0), 0);
        }
    }

    if (erase) {
        for (column = left + 1; column < right; column++) {
            for (row = top + 1; row < bottom; row++) {
                SCREEN[row * SCREEN_ROW_BYTES + column * 2 + 0] = 0x20;
                SCREEN[row * SCREEN_ROW_BYTES + column * 2 + 1] = 0;
            }
        }
    }

    for (column = left; column < right; column++) {
        SCREEN[top * SCREEN_ROW_BYTES + column * 2] = 0x40; // horizontal line, centred
        SCREEN[top * SCREEN_ROW_BYTES + column * 2 + 1] = 0;
        SCREEN[bottom * SCREEN_ROW_BYTES + column * 2] = 0x40; // horizontal line, centred
        SCREEN[bottom * SCREEN_ROW_BYTES + column * 2 + 1] = 0;
    }

    for (row = top; row < bottom; row++) {
        SCREEN[row * SCREEN_ROW_BYTES + left * 2] = 0x42; // vertical line, centred
        SCREEN[row * SCREEN_ROW_BYTES + left * 2 + 1] = 0;
        SCREEN[row * SCREEN_ROW_BYTES + right * 2] = 0x42; // vertical line, centred
        SCREEN[row * SCREEN_ROW_BYTES + right * 2 + 1] = 0;
    }
    SCREEN[top * SCREEN_ROW_BYTES + left * 2] = 0x55;   // top left corner
    SCREEN[top * SCREEN_ROW_BYTES + right * 2] = 73;    // top right corner
    SCREEN[bottom * SCREEN_ROW_BYTES + left * 2] = 74;  // bottom left corner
    SCREEN[bottom * SCREEN_ROW_BYTES + right * 2] = 75; // bottom right corner
    SCREEN[top * SCREEN_ROW_BYTES + left * 2 + 1] = 0;
    SCREEN[top * SCREEN_ROW_BYTES + right * 2 + 1] = 0;
    SCREEN[bottom * SCREEN_ROW_BYTES + left * 2 + 1] = 0;
    SCREEN[bottom * SCREEN_ROW_BYTES + right * 2 + 1] = 0;
}

static void write_text(
    unsigned char column, unsigned char row, unsigned char colour, const char* text) {
    unsigned char screen_column;
    unsigned char character;
    for (screen_column = column; text[screen_column - column]; screen_column++) {
        character = text[screen_column - column];
        if (character > 0x60) {
            character -= 0x60;
        }
        if (character > 0x40) {
            character -= 0x40;
        }
        SCREEN[row * SCREEN_ROW_BYTES + screen_column * 2 + 0] = character;
        SCREEN[row * SCREEN_ROW_BYTES + screen_column * 2 + 1] = 0;
        colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + screen_column * 2 + 0), 0x00);
        colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + screen_column * 2 + 1), colour);
    }
}

static void input_text(unsigned char column,
    unsigned char row,
    unsigned char width,
    unsigned char colour,
    char* into) {
    unsigned char offset = 0;
    unsigned char screen_column;
    unsigned char character;
    for (screen_column = column; screen_column < (column + width); screen_column++) {
        SCREEN[row * SCREEN_ROW_BYTES + screen_column * 2 + 0] = ' ';
        SCREEN[row * SCREEN_ROW_BYTES + screen_column * 2 + 1] = 0;
        colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + screen_column * 2 + 0), 0x00);
        colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + screen_column * 2 + 1), colour);
    }

    into[0] = 0;

    while (1) {
        // Enable cursor on current char
        colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + (column + offset) * 2 + 1), colour | 0x30);

        character = ASCIIKEY;
        if ((character >= 0x41 && character <= 0x5a) || (character >= 0x61 && character <= 0x7a) ||
            (character >= 0x30 && character <= 0x39)) {
            if (offset < width) {
                into[offset] = character;
                // ASCII to screen code conversion
                if (character > 0x60) {
                    character -= 0x60;
                }
                SCREEN[row * SCREEN_ROW_BYTES + (column + offset) * 2 + 0] = character;
                colour_poke((uint16_t)(row * SCREEN_ROW_BYTES + (column + offset) * 2 + 1), colour);
                offset++;
            }
        } else {
            switch (character) {
                case KEY_DELETE: // delete
                    // XXX actually copy chars down, instead of just erasing from
                    // end of line, and allow cursor left and right
                    SCREEN[row * SCREEN_ROW_BYTES + (column + offset) * 2 + 0] = ' ';
                    colour_poke(
                        (uint16_t)(row * SCREEN_ROW_BYTES + (column + offset) * 2 + 1), colour);
                    if (offset) {
                        offset--;
                    }
                    SCREEN[row * SCREEN_ROW_BYTES + (column + offset) * 2 + 0] = ' ';
                    colour_poke(
                        (uint16_t)(row * SCREEN_ROW_BYTES + (column + offset) * 2 + 1), colour);
                    break;
                case KEY_RUN_STOP:
                    into[0] = 0;
                    ASCIIKEY = 0;
                    return;
                case KEY_RETURN:
                    into[offset] = 0;
                    ASCIIKEY = 0;
                    return;
                default:
                    break;
            }
        }
        if (character) {
            ASCIIKEY = 0;
        }
    }
}

// clang-format off
static const uint8_t BAM_SECTOR1[0x100] = {
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

static uint8_t to_hex(uint8_t i) {
    if (i < 10) {
        return '0' + i;
    }
    return 0x41 + i - 10;
}

static void format_disk_image(uint32_t file_sector, char* diskname, bool is_d65) {
    unsigned char i;
    uint32_t s;
    const struct DiskGeometry* geom = disk_geometry(is_d65);
    const uint32_t sect_count = disk_total_sectors(geom);

    /* Thousands of sector operations behind a screen that says only "CREATING
     * IMAGE...".  The card answers instantly under emulation and does not on
     * hardware, where a still border is indistinguishable from a hang, so let
     * the border move while the controller is busy. */
    sdcard_visual_feedback(2);

    /* Multi-sector writes look like the obvious speed-up here and are not: a
     * multi-block run cannot read, and it is the read before each write that
     * skips the write entirely when the sector already holds zeros.  Measured
     * over a whole D81 on a clean card, that is 1600 writes avoided out of
     * 1600.  See test/verify_sdcount_xemu.py. */
    clear_sector_buffer();
    for (s = 0; s < sect_count; s++) {
        sdcard_writesector(file_sector + s, 0);
    }

    sdcard_visual_feedback(0);
    VICIV.bordercol = SchemeBorder;

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
    const uint8_t id_lo = to_hex(i & 0xf);
    const uint8_t id_hi = to_hex(i >> 4);
    sector_buffer[0x16] = id_lo;
    sector_buffer[0x17] = id_hi;

    sector_buffer[0x18] = 0xa0;

    // DOS type
    sector_buffer[0x19] = 0x31;
    sector_buffer[0x1A] = 0x44;

    sector_buffer[0x1B] = 0xa0;
    sector_buffer[0x1C] = 0xa0;

    lcopy((long)BAM_SECTOR1, (long)&sector_buffer[0x100], 0x100);

    // Disk ID in BAM
    sector_buffer[0x104] = id_lo;
    sector_buffer[0x105] = id_hi;

    const uint32_t bam_sector = file_sector + disk_bam_sector(geom);
    sdcard_writesector(bam_sector, 0);

    clear_sector_buffer();
    lcopy((long)BAM_SECTOR1, (long)sector_buffer, 0x100);
    // Disk ID in BAM
    sector_buffer[0x004] = id_lo;
    sector_buffer[0x005] = id_hi;
    sector_buffer[0x101] = 0xff;
    // Link to first sector of dir
    sector_buffer[0x000] = 0x00;
    sector_buffer[0x001] = 0xFF;
    // Mark all sectors free in 2nd half of disk
    sector_buffer[0x0FA] = 40;
    sector_buffer[0x0FB] = 0xff;

    sdcard_writesector(bam_sector + 1, 0);
}

static void do_make_disk_image(bool is_d65, uint8_t drive_id) {
    char diskname[16 + 1];
    char filename[16 + 1];
    unsigned char filename_len;
    uint32_t file_sector;

    /* Both conditions: the return catches a card with no MBR signature, while
     * fat1_sector catches a valid MBR carrying no FAT32 partition.  The global
     * is never reset, so on a second call it would otherwise still hold the
     * previous card's value. */
    if (fat32_open_file_system() != FreezerOk || !fat1_sector) {
        draw_box(10, 8, 30, 13, SchemeError, 1);
        write_text(11, 9, SchemeNotice, "COULD NOT FIND SD CARD");
        while (!ASCIIKEY) {
        }
        ASCIIKEY = 0;
        return;
    }

    draw_box(10, 8, 30, 14, SchemeAccent, 1);
    write_text(11, 9, SchemeAccent, "ENTER NAME FOR");
    if (is_d65) {
        write_text(11, 10, SchemeAccent, "HD (D65) IMAGE:");
    } else {
        write_text(11, 10, SchemeAccent, "DD (D81) IMAGE:");
    }
    input_text(11, 12, 8, SchemeText, filename);
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

    const struct DiskGeometry* geom = disk_geometry(is_d65);
    strcpy(&filename[filename_len], geom->extension);

    draw_box(10, 8, 30, 14, SchemeNotice, 1);
    write_text(11, 9, SchemeNotice, "CREATING IMAGE...");

    // Actually create the file
    file_sector =
        fat32_create_contiguous_file(filename, disk_total_sectors(geom) * (uint32_t)SD_SECTOR_SIZE);
    if (!file_sector) {
        // Error making file
        draw_box(10, 8, 30, 14, SchemeError, 1);
        write_text(11, 9, SchemeError, "Error creating file");
        write_text(11, 12, SchemeText, "Press almost any key...");
        while (!ASCIIKEY) {
        }
        ASCIIKEY = 0;
    } else {
        // File creation succeeded

        // Write header, BAM and zero out directory track
        write_text(11, 10, SchemeAccent, "FORMATTING IMAGE...");
        format_disk_image(file_sector, diskname, is_d65);

        draw_box(8, 8, 32, 14, SchemeHighlight, 1);
        write_text(9, 9, SchemeHighlight, "Created disk image");

        // now mount the new image on the drive_id we got
        mega65_dos_attach(filename, drive_id);
        // store mount to freeze slot
        copy_imageproc_to_freezeregion(drive_id, 0);

        write_text(9, 12, SchemeText, "Press almost any key...");

        while (!ASCIIKEY) {
        }
        ASCIIKEY = 0;
    }
}

int main(void) {
    freezer_tool_start();

    // Now find the start sector of the slot, and make a copy for safe keeping
    slot_number = 0;
    freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);

    setup_menu_screen_16bit();

    request_freeze_region_list();

    do_make_disk_image(tool_density[0] != 0, tool_drive_id[0] != 0);

    mega65_dos_exechelper("FREEZER.M65");
}
