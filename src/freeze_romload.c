#include "fdisk_fat32.h"
#include "fdisk_hal.h"
#include "fdisk_memory.h"
#include "fdisk_screen.h"
#include "freezer.h"
#include "freezer_common.h"
#include "mega65_regs.h"

// Same listing buffer and 64-byte stride as the disk chooser: at most
// 0xffff/64 = 1023 entries, and 1023 << 6 = 65472 fits an unsigned 16-bit
// value.  file_count and selection_number are short, so the plain product
// overflowed *signed* int at entry 512.
#define DIR_NAME_BUF 0x40000L
#define DIR_ENTRY_INDEX(n) ((uint16_t)(n) << 6)

#include <mega65.h>
#include <stdio.h>
#include <string.h>

short file_count = 0;
short selection_number = 0;
short display_offset = 0;

unsigned char buffer[512];

char reading_disk_list_message[] = "SCANNING DIRECTORY ...";

char diskchooser_instructions[] = "SELECT A ROM FILE, PRESS RETURN TO LOAD,"
                                  "  OR PRESS RUN/STOP TO LEAVE UNCHANGED  ";

char rom_reset_screen[] = "YOU HAVE LOADED THE ROM FILE            "
                          "                                        "
                          "                                        "
                          "    ROM VERSION:                        "
                          "                                        "
                          "INTO FROZEN MEMORY, REPLACING THE WHOLE "
                          "ROM STORED THERE.                       "
                          "                                        "
                          "IT IS PROBABLY UNSAFE TO JUST RESUME THE"
                          "SYSTEM, A FULL RESET IS RECOMMENDED.    "
                          "                                        "
                          "        RESET SYSTEM NOW? (Y/N)         ";

// clang-format off
unsigned char normal_row[40] = {
  0, 1, 0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 1, 0, 1 };

unsigned char highlight_row[40] = {
  0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21,
  0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21,
  0, 0x21, 0, 0x21, 0, 0x21, 0, 0x21 };

unsigned char dir_line_colour[40] = {
  0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe,
  0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe,
  0, 0xe, 0, 0xe, 0, 0xe, 0, 0xe };
// clang-format on

char rom_name_return[32];

#ifdef WITH_JOYSTICK
unsigned char joy_to_key_disk[32] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0x0d, // With fire pressed
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0x9d,
    0,
    0,
    0,
    0x1d,
    0,
    0x11,
    0x91,
    0 // without fire
};
#endif

char draw_directory_entry(unsigned char screen_row) {
    char type;
    char firsta0 = 1;
    char invalid = 0;
    unsigned char i;
    unsigned char c;
    // Skip first 5 bytes
    for (i = 0; i < 2; i++) {
        (void)F011_DATA;
    }
    type = F011_DATA;
    if (!(type & 0xf)) {
        invalid = 1;
    }
    for (i = 0; i < 2; i++) {
        (void)F011_DATA;
    }
    // Then draw the 16 chars with quotes
    POKE(SCREEN_ADDRESS + (screen_row * SCREEN_ROW_BYTES) + (21 * 2), '"');
    for (i = 0; i < 16; i++) {
        c = F011_DATA;
        if (!c) {
            invalid = 1;
        }
        if (firsta0 && (c == 0xa0)) {
            POKE(SCREEN_ADDRESS + (screen_row * SCREEN_ROW_BYTES) + (22 * 2) + (i * 2), 0x22);
            firsta0 = 0;
        } else {
            if (c >= 'A' && c <= 'Z') {
                c &= 0x1f;
            }
            if (c >= 'a' && c <= 'z') {
                c &= 0x1f;
            }
            POKE(SCREEN_ADDRESS + (screen_row * SCREEN_ROW_BYTES) + (22 * 2) + (i * 2), c & 0x7f);
        }
    }
    if (firsta0) {
        POKE(SCREEN_ADDRESS + (screen_row * SCREEN_ROW_BYTES) + (38 * 2), '"');
    }
    if (type & 0x40) {
        POKE(SCREEN_ADDRESS + (screen_row * SCREEN_ROW_BYTES) + (39 * 2), '<');
    }
    // Marks an entry whose type carries none of the high-nibble bits.  Written
    // `!type & 0xf0` here and in cc65, which parses as `(!type) & 0xf0` and is
    // always 0, so the '*' never drew.
    if (!(type & 0xf0)) {
        POKE(SCREEN_ADDRESS + (screen_row * SCREEN_ROW_BYTES) + (39 * 2), '*');
    }

    // Read the rest of the entry to advance buffer pointer nicely
    for (i = 0; i < 11; i++) {
        (void)F011_DATA;
    }

    if (invalid) {
        // Erase whatever we drew
        for (i = 21; i < 40; i++) {
            POKE(SCREEN_ADDRESS + (screen_row * SCREEN_ROW_BYTES) + (i * 2), ' ');
        }
    } else {
        lcopy((uint32_t)dir_line_colour,
            COLOUR_RAM_ADDRESS + (screen_row * SCREEN_ROW_BYTES + (21 * 2)),
            19 * 2);
    }

    return invalid;
}

void clear_screen(unsigned char lines) {
    POKE(SCREEN_ADDRESS + 0, ' ');
    POKE(SCREEN_ADDRESS + 1, 0);
    POKE(SCREEN_ADDRESS + 2, ' ');
    POKE(SCREEN_ADDRESS + 3, 0);
    lcopy(SCREEN_ADDRESS, SCREEN_ADDRESS + 4, 40 * 2 * lines - 4);
    lpoke(COLOUR_RAM_ADDRESS + 0, 0);
    lpoke(COLOUR_RAM_ADDRESS + 1, 1);
    lpoke(COLOUR_RAM_ADDRESS + 2, 0);
    lpoke(COLOUR_RAM_ADDRESS + 3, 1);
    lcopy(COLOUR_RAM_ADDRESS, COLOUR_RAM_ADDRESS + 4, 40 * 2 * lines - 4);
}

void copy_line_to_screen(long dest, char* src, uint16_t length) {

    for (uint16_t i = 0; i < length && src[i] != 0; i++) {
        POKE(dest + (i << 1) + 0, petscii_to_screen(src[i]));
        POKE(dest + (i << 1) + 1, 0);
    }
}

void draw_file_list(void) {
    unsigned addr = SCREEN_ADDRESS + 4;
    unsigned char i;
    unsigned char x;
    unsigned char name[64];
    // First, clear the screen
    clear_screen(23);

    // Draw instructions
    copy_line_to_screen(SCREEN_ADDRESS + 23 * SCREEN_ROW_BYTES, diskchooser_instructions, 80);
    lcopy((long)highlight_row, COLOUR_RAM_ADDRESS + (23 * SCREEN_ROW_BYTES) + 0, 40);
    lcopy((long)highlight_row, COLOUR_RAM_ADDRESS + (23 * SCREEN_ROW_BYTES) + 40, 40);
    lcopy((long)highlight_row, COLOUR_RAM_ADDRESS + (24 * SCREEN_ROW_BYTES) + 0, 40);
    lcopy((long)highlight_row, COLOUR_RAM_ADDRESS + (24 * SCREEN_ROW_BYTES) + 40, 40);

    for (i = 0; i < 23; i++) {
        if ((display_offset + i) < file_count) {
            // Real line
            lcopy(0x40000U + ((display_offset + i) << 6), (uint32_t)name, 64);

            for (x = 0; x < 32; x++) {
                if ((name[x] >= 'A' && name[x] <= 'Z') || (name[x] >= 'a' && name[x] <= 'z')) {
                    POKE(addr + (x << 1), name[x] & 0x1f);
                } else {
                    POKE(addr + (x << 1), name[x]);
                }
            }
        }
        /*    else {
              // Blank dummy entry
              for (x = 0; x < 40; x++)
                POKE(addr + (x << 1), ' ');
            }*/
        if ((display_offset + i) == selection_number) {
            // Highlight the row
            lcopy((long)highlight_row, COLOUR_RAM_ADDRESS + (i * SCREEN_ROW_BYTES), 40);
        } else {
            // Normal row
            lcopy((long)normal_row, COLOUR_RAM_ADDRESS + (i * SCREEN_ROW_BYTES), 40);
        }
        addr += (40 * 2);
    }
}

void scan_directory(void) {
    unsigned char x;
    unsigned char dir;
    short last_dir = -1;
    short dir_pos;
    short i;
    struct m65_dirent* dirent;

    file_count = 0;

    closeall();

    dir = opendir();
    dirent = readdir(dir);
    while (dirent && ((uint16_t)dirent != 0xffffU)) {

        x = (unsigned char)strlen(dirent->d_name);
        // only accept 32 characters max!
        if (x > 32) {
            goto next_entry;
        }

        // check DIR attribute of dirent
        if (dirent->d_type & 0x10) {
            // File is a directory
            // limit filename length and skip '.' directory
            if (strcmp(dirent->d_name, ".") != 0) {
                // keep directories at the top, and
                // always put ROMS dir at the very top
                if (!strcmp(dirent->d_name, "ROMS")) {
                    dir_pos = 0;
                } else {
                    dir_pos = last_dir + 1;
                }
                if (file_count && dir_pos != file_count) {
                    for (i = file_count - 1; i >= dir_pos; i--) {
                        lcopy(DIR_NAME_BUF + DIR_ENTRY_INDEX(i),
                            DIR_NAME_BUF + 0x40 + DIR_ENTRY_INDEX(i),
                            64); // can't reverse copy!
                    }
                }
                lfill(DIR_NAME_BUF + DIR_ENTRY_INDEX(dir_pos), ' ', 64);
                lcopy((long)&dirent->d_name[0], DIR_NAME_BUF + 1 + DIR_ENTRY_INDEX(dir_pos), x);
                // Put / at the start of directory names to make them obviously different
                lpoke(DIR_NAME_BUF + DIR_ENTRY_INDEX(dir_pos), '/');
                last_dir++;
                file_count++;
            }
        } else if (x > 4 &&
            ((!strncmp(&dirent->d_name[x - 4], ".ROM", 4)) ||    // ROM Files
                (!strncmp(&dirent->d_name[x - 4], ".BIN", 4)) || // ROM Files
                (!strncmp(&dirent->d_name[x - 4], ".CHR", 4)) || // 8x8 CHaRacter Set FONT files
                (!strncmp(
                    &dirent->d_name[x - 4], ".TCR", 4)) || // 8x16 Tall ChaRacter Set FONT files
                (!strncmp(&dirent->d_name[x - 4], ".rom", 4)) || // ROM Files
                (!strncmp(&dirent->d_name[x - 4], ".bin", 4)) || // ROM Files
                (!strncmp(&dirent->d_name[x - 4], ".chr", 4)) || // 8x8 CHaRacter Set FONT files
                (!strncmp(
                    &dirent->d_name[x - 4], ".tcr", 4)))) { // 8x16 Tall ChaRacter Set FONT files
            // File is a ROM or a CHaRset
            lfill(DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), ' ', 64);
            lcopy((long)&dirent->d_name[0], DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), x);
            file_count++;
        }

    next_entry:
        dirent = readdir(dir);
    }

    closedir(dir);
}

/*
 * uchar freeze_load_romarea()
 *
 * lets the user select a ROM like file from the SDcard
 * and loads it into frozen memory
 *
 * returns 1 with the whole ROM was replaced and the user
 * should be prompted for a direct reset
 */
unsigned char freeze_load_romarea(void) {
    unsigned char x;

    file_count = 0;
    selection_number = 0;
    display_offset = 0;

    // First, clear the screen
    POKE(SCREEN_ADDRESS + 0, ' ');
    POKE(SCREEN_ADDRESS + 1, 0);
    POKE(SCREEN_ADDRESS + 2, ' ');
    POKE(SCREEN_ADDRESS + 3, 0);
    lcopy(SCREEN_ADDRESS, SCREEN_ADDRESS + 4, 40 * 2 * 25 - 4);

    for (x = 0; reading_disk_list_message[x]; x++) {
        POKE(SCREEN_ADDRESS + 12 * 40 * 2 + (9 * 2) + (x * 2), reading_disk_list_message[x] & 0x3f);
    }

    scan_directory();

    // If we didn't find any disk images, then just return
    if (!file_count) {
        return 0;
    }

    // Okay, we have some disk images, now get the user to pick one!
    draw_file_list();
    while (1) {
        x = ASCIIKEY;
        if (x) {
            ASCIIKEY = 0;
        }

#ifdef WITH_JOYSTICK
        if (!x) {
            // We use a simple lookup table to do this
            x = joy_to_key_disk[CIA1.pra & CIA1.prb & 0x1f];
            // Then wait for joystick to release
            while ((CIA1.pra & CIA1.prb & 0x1f) != 0x1f)
                continue;
        }
#endif

        switch (x) {
            case 0x03: // RUN-STOP = make no change
            case 0x1b: // ESC
                return 0;
            case 0x5f: // <- key at top left of key board
                // Go back up one directory

                mega65_dos_chdir((unsigned char*)"..");
                file_count = 0;
                selection_number = 0;
                display_offset = 0;
                scan_directory();
                draw_file_list();

                break;
            case 0x0d:
            case 0x21: // Return = select this file.
                // Copy name out
                lcopy(DIR_NAME_BUF + DIR_ENTRY_INDEX(selection_number),
                    (uint32_t)rom_name_return,
                    32);
                // Then null terminate it
                for (x = 31; x; x--) {
                    if (rom_name_return[x] == ' ') {
                        rom_name_return[x] = 0;
                    } else {
                        break;
                    }
                }

                // Is it a directory?
                if (rom_name_return[0] == '/') {
                    // Its a directory
                    mega65_dos_chdir((unsigned char*)&rom_name_return[1]);
                    file_count = 0;
                    selection_number = 0;
                    display_offset = 0;
                    scan_directory();
                    draw_file_list();
                } else {
                    // XXX - Actually do loading of ROM / ROM diff file
                    VICIV.bordercol = 0;
                    if (!strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".ROM") ||
                        !strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".BIN") ||
                        !strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".rom") ||
                        !strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".bin")) {
                        int s;
                        // Load normal ROM file
                        // Begin by loading the file at $40000-$5FFFF
                        read_file_from_sdcard(rom_name_return, 0x40000L);

                        // Then progressively save it into the frozen memory
                        request_freeze_region_list();
                        freeze_slot_start_sector =
                            read_freeze_slot_start_sector(0); // we only work on slot 0!

                        for (s = 0; s < 256; s++) { // ROM is 128k, devided by 512 byte sectors is
                                                    // 256 sectors to load
                            // Write each sector to frozen memory
                            VICIV.bordercol = (VICIV.bordercol + 1) & 0xf;
                            lcopy(0x40000L + 512L * (long)s, (long)buffer, 512);
                            freeze_store_sector(0x20000L + ((long)s) * 512L, buffer);
                        }
                        VICIV.bordercol = 6;

                        return 1;
                    }

                    if (!strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".CHR") ||
                        !strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".TCR") ||
                        !strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".chr") ||
                        !strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".tcr")) {
                        unsigned char cg_7a_set = 0;
                        unsigned char cg_7a_mask = 0xff;
                        unsigned char cg_54_set = 0;
                        unsigned char cg_54_mask = 0xff;

                        // Load CHARSET to chargen WOM
                        read_file_from_sdcard(rom_name_return, 0x40000L);
                        lcopy(0x40000L, CHARGEN_ADDRESS, 4096);

                        request_freeze_region_list();
                        freeze_slot_start_sector =
                            read_freeze_slot_start_sector(0); // we only work on slot 0!

                        if (freeze_region_flags & FreezeRegionHasChargen) {
                            // only put that into the slot, if HYPPO supports it!
                            for (uint16_t i = 0; i < 8; i++) {
                                lcopy(0x40000L + 512L * i, (long)sector_buffer, 512);
                                freeze_store_sector(CHARGEN_ADDRESS + 512L * i, NULL);
                            }
                        }

                        // set or reset TALL character bit depending on charset extension
                        if (!strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".TCR")) {
                            // switch palemu off and enable CHARY16
                            cg_54_set |= 0x20;
                            cg_54_mask ^= 0x20;
                            cg_7a_set |= 0x10;
                            cg_7a_mask ^= 0x10;
                        } else {
                            cg_7a_set |= 0x00;
                            cg_7a_mask ^= 0x10;
                        }
                        freeze_poke(0xFFD307AL, cg_54_set | (freeze_peek(0xFFD307AL) & cg_54_mask));
                        freeze_poke(0xFFD307AL, cg_7a_set | (freeze_peek(0xFFD307AL) & cg_7a_mask));
                        lpoke(0xFFD307AL, cg_54_set | (lpeek(0xFFD307AL) & cg_54_mask));
                        lpoke(0xFFD307AL, cg_7a_set | (lpeek(0xFFD307AL) & cg_7a_mask));

                        return 0; // no reset needed, most probably...
                    }
                }
                break;

            case 0x13: // HOME
                selection_number = 0;
                break;
            case 0x93: // Shift-HOME
                selection_number = file_count - 1;
                break;
            case 0x1d: // Cursor right, next page
                selection_number += 22;
            case 0x11: // Cursor down, one down
                selection_number++;
                if (selection_number >= file_count) {
                    selection_number = file_count - 1;
                }
                break;
            case 0x9d: // Cursor left, prev page
                selection_number -= 22;
            case 0x91: // Cursor up, one up
                selection_number--;
                if (selection_number < 0) {
                    selection_number = 0;
                }
                break;
            default:
                break;
        }

        // Adjust display position
        if (selection_number < display_offset) {
            display_offset = selection_number;
        }
        if (selection_number > (display_offset + 22)) {
            display_offset = selection_number - 22;
        }
        if (display_offset > (file_count - 22)) {
            display_offset = file_count - 22;
        }
        if (display_offset < 0) {
            display_offset = 0;
        }

        if (x) {
            draw_file_list();
        }
    }

    return 0;
}

void user_reset_prompt(void) {
    unsigned char x = 0;

    clear_screen(25);
    copy_line_to_screen(SCREEN_ADDRESS + 80, rom_reset_screen, 12 * SCREEN_ROW_BYTES);
    copy_line_to_screen(SCREEN_ADDRESS + 3 * SCREEN_ROW_BYTES + 8, rom_name_return, 32);
    copy_line_to_screen(SCREEN_ADDRESS + 4 * SCREEN_ROW_BYTES + 34, detect_rom(), 11);

    while (x != 'Y' && x != 'y' && x != 'N' && x != 'n') {
        x = ASCIIKEY;
        if (x) {
            ASCIIKEY = 0;
        }

#ifdef WITH_JOYSTICK
        if (!x) {
            // We use a simple lookup table to do this
            x = joy_to_key_disk[CIA1.pra & CIA1.prb & 0x1f];
            // Then wait for joystick to release
            while ((CIA1.pra & CIA1.prb & 0x1f) != 0x1f)
                continue;
            // translate joystick to keys
            if (x == 0xd) // fire is Y
                x = 'Y';
            else if (x == 0x9d) // left is abort
                x = 'N';
        }
#endif
    }

    if (x == 'y' || x == 'Y') {
        freeze_poke(0xFFD3640U + 8, freeze_peek(0x2FFFCL));
        freeze_poke(0xFFD3640U + 9, freeze_peek(0x2FFFDL));
        // Reset $01 port values
        freeze_poke(0xFFD3640U + 0x10, 0x3f);
        freeze_poke(0xFFD3640U + 0x11, 0x3f);
        // disable interrupts, clear decimal mode
        freeze_poke(0xFFD3640U + 0x07, 0xe7);
        // Clear memory mapping
        for (x = 0x0a; x <= 0x0f; x++) {
            freeze_poke(0xFFD3640U + x, 0);
        }
        // Turn off extended graphics mode, only keep palemu
        freeze_poke(0xFFD3054U, freeze_peek(0xFFD3054U) & 0x20);
        unfreeze_slot(0);

        while (1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0xf;
        }
    }
}

// returns 1 if rom was changed and user skipped reset
unsigned char do_rom_loader(void) {
    unsigned char changed = 0;

    // Get user to select a ROM file from the SD card
    // This loads it into RAM, and then writes it out into the freeze slot
    if (freeze_load_romarea()) {
        user_reset_prompt();
        changed = 1;
    }

    // need to go up to root dir, or we can't load FREEZER again!
    mega65_dos_cdroot();

    // Return, so that control can go back to the freezer
    return changed;
}
