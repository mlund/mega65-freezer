#include "colours.h"
#include "common.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

// Same listing buffer and 64-byte stride as the disk chooser: at most
// 0xffff/64 = 1023 entries, and 1023 << 6 = 65472 fits an unsigned 16-bit
// value.  file_count and selection_number are short, so the product must be
// widened: a short promotes to *signed* int, which stops at 32767, and entry
// 512 onwards would reach the sign bit.
#define DIR_NAME_BUF 0x40000L
#define DIR_ENTRY_INDEX(n) ((uint16_t)(n) << 6)

#include <mega65.h>
#include <stdio.h>
#include <string.h>

static short file_count = 0;
static short selection_number = 0;
static short display_offset = 0;

static unsigned char buffer[SD_SECTOR_SIZE];

static char reading_disk_list_message[] = "SCANNING DIRECTORY ...";

static char diskchooser_instructions[] = "SELECT A ROM FILE, PRESS RETURN TO LOAD,"
                                         "  OR PRESS RUN/STOP TO LEAVE UNCHANGED  ";

static char rom_reset_screen[] = "YOU HAVE LOADED THE ROM FILE            "
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
/* One colour-RAM pair -- attribute byte, colour byte -- per screen column, so
 * the count is columns and the array is twice that many bytes.  A macro because
 * an initialiser cannot call a function. */
#define COLOUR_PAIRS_10(c) \
  0, (c), 0, (c), 0, (c), 0, (c), 0, (c), \
  0, (c), 0, (c), 0, (c), 0, (c), 0, (c)
#define COLOUR_PAIRS_20(c) COLOUR_PAIRS_10(c), COLOUR_PAIRS_10(c)

static unsigned char normal_row[40] = { COLOUR_PAIRS_20(SchemeText) };
static unsigned char highlight_row[40] = { COLOUR_PAIRS_20(SchemeSelected | AttribReverse) };

#undef COLOUR_PAIRS_20
#undef COLOUR_PAIRS_10
// clang-format on

static char rom_name_return[32];

void clear_screen(unsigned char lines) {
    SCREEN[0] = ' ';
    SCREEN[1] = 0;
    SCREEN[2] = ' ';
    SCREEN[3] = 0;
    lcopy(SCREEN_ADDRESS, SCREEN_ADDRESS + 4, 40 * 2 * lines - 4);
    lpoke(COLOUR_RAM_ADDRESS + 0, 0);
    lpoke(COLOUR_RAM_ADDRESS + 1, SchemeText);
    lpoke(COLOUR_RAM_ADDRESS + 2, 0);
    lpoke(COLOUR_RAM_ADDRESS + 3, SchemeText);
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
    mega65_h_dirent* const dirent = HYPPO_DIRENT;

    file_count = 0;

    mega65_h_closeall();

    if (mega65_h_opendir(&dir) != MEGA65_H_OK) {
        return;
    }
    while (mega65_h_readdir(dir, dirent) == MEGA65_H_OK) {

        x = dirent->name_len;
        // only accept 32 characters max!
        if (x > 32) {
            continue;
        }

        // check DIR attribute of dirent
        if (dirent->attributes & MEGA65_H_ATTR_SUBDIR) {
            // File is a directory
            // limit filename length and skip '.' directory
            if (!dirent_is_dot(dirent->long_name, x)) {
                // keep directories at the top, and
                // always put ROMS dir at the very top
                if (!strcmp(dirent->long_name, "ROMS")) {
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
                lcopy((long)&dirent->long_name[0], DIR_NAME_BUF + 1 + DIR_ENTRY_INDEX(dir_pos), x);
                // Put / at the start of directory names to make them obviously different
                lpoke(DIR_NAME_BUF + DIR_ENTRY_INDEX(dir_pos), '/');
                last_dir++;
                file_count++;
            }
        } else if (x > 4 &&
            ((!strncmp(&dirent->long_name[x - 4], ".ROM", 4)) ||    // ROM Files
                (!strncmp(&dirent->long_name[x - 4], ".BIN", 4)) || // ROM Files
                (!strncmp(&dirent->long_name[x - 4], ".CHR", 4)) || // 8x8 CHaRacter Set FONT files
                (!strncmp(
                    &dirent->long_name[x - 4], ".TCR", 4)) || // 8x16 Tall ChaRacter Set FONT files
                (!strncmp(&dirent->long_name[x - 4], ".rom", 4)) || // ROM Files
                (!strncmp(&dirent->long_name[x - 4], ".bin", 4)) || // ROM Files
                (!strncmp(&dirent->long_name[x - 4], ".chr", 4)) || // 8x8 CHaRacter Set FONT files
                (!strncmp(
                    &dirent->long_name[x - 4], ".tcr", 4)))) { // 8x16 Tall ChaRacter Set FONT files
            // File is a ROM or a CHaRset
            lfill(DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), ' ', 64);
            lcopy((long)&dirent->long_name[0], DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), x);
            file_count++;
        }
    }

    mega65_h_closedir(dir);
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
    SCREEN[0] = ' ';
    SCREEN[1] = 0;
    SCREEN[2] = ' ';
    SCREEN[3] = 0;
    lcopy(SCREEN_ADDRESS, SCREEN_ADDRESS + 4, 40 * 2 * 25 - 4);

    for (x = 0; reading_disk_list_message[x]; x++) {
        SCREEN[12 * 40 * 2 + (9 * 2) + (x * 2)] = reading_disk_list_message[x] & 0x3f;
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

        switch (x) {
            case KEY_RUN_STOP: // RUN-STOP = make no change
            case KEY_ESC:      // ESC
                return 0;
            case KEY_LEFT_ARROW: // <- key at top left of key board
                // Go back up one directory

                mega65_dos_chdir("..");
                file_count = 0;
                selection_number = 0;
                display_offset = 0;
                scan_directory();
                draw_file_list();

                break;
            case KEY_RETURN:
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
                    mega65_dos_chdir(&rom_name_return[1]);
                    file_count = 0;
                    selection_number = 0;
                    display_offset = 0;
                    scan_directory();
                    draw_file_list();
                } else {
                    // XXX - Actually do loading of ROM / ROM diff file
                    VICIV.bordercol = SchemeBorderBusy;
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
                            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
                            lcopy(0x40000L + (long)SD_SECTOR_SIZE * (long)s,
                                (long)buffer,
                                SD_SECTOR_SIZE);
                            freeze_store_sector(0x20000L + ((long)s) * SD_SECTOR_SIZE, buffer);
                        }
                        VICIV.bordercol = SchemeBorder;

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
                                lcopy(0x40000L + (long)SD_SECTOR_SIZE * i,
                                    (long)sector_buffer,
                                    SD_SECTOR_SIZE);
                                freeze_store_sector(
                                    CHARGEN_ADDRESS + (long)SD_SECTOR_SIZE * i, NULL);
                            }
                        }

                        // set or reset TALL character bit depending on charset extension
                        if (!strcmp(&rom_name_return[strlen(rom_name_return) - 4], ".TCR")) {
                            /* $D054.5 is PALEMU and $D07A.4 is CHARY16, so these
                             * two go to different registers.  Both writes used to
                             * land on $D07A: PALEMU was never touched and $D07A.5
                             * -- NOBUGCOMPAT, which the core marks deprecated --
                             * was set instead, and nothing ever cleared it again.
                             *
                             * The bit is left as it was written, which sets
                             * PALEMU where the old comment said "switch palemu
                             * off".  Nothing settles which is meant: .TCR is this
                             * program's own convention, named in no core, ROM,
                             * book or tool, so there is no specification to read
                             * and no such file on a stock card to try. */
                            cg_54_set |= 0x20;
                            cg_54_mask ^= 0x20;
                            cg_7a_set |= 0x10;
                            cg_7a_mask ^= 0x10;
                        } else {
                            cg_7a_set |= 0x00;
                            cg_7a_mask ^= 0x10;
                        }
                        freeze_io_poke(0x3054, cg_54_set | (freeze_io_peek(0x3054) & cg_54_mask));
                        freeze_io_poke(0x307a, cg_7a_set | (freeze_io_peek(0x307a) & cg_7a_mask));
                        lpoke(0xFFD3054L, cg_54_set | (lpeek(0xFFD3054L) & cg_54_mask));
                        lpoke(0xFFD307AL, cg_7a_set | (lpeek(0xFFD307AL) & cg_7a_mask));

                        return 0; // no reset needed, most probably...
                    }
                }
                break;

            case KEY_HOME: // HOME
                selection_number = 0;
                break;
            case KEY_SHIFT_HOME: // Shift-HOME
                selection_number = file_count - 1;
                break;
            case KEY_CURSOR_RIGHT: // Cursor right, next page
                selection_number += 22;
            case KEY_CURSOR_DOWN: // Cursor down, one down
                selection_number++;
                if (selection_number >= file_count) {
                    selection_number = file_count - 1;
                }
                break;
            case KEY_CURSOR_LEFT: // Cursor left, prev page
                selection_number -= 22;
            case KEY_CURSOR_UP: // Cursor up, one up
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
    }

    if (x == 'y' || x == 'Y') {
        freeze_reset_cpu_state();
        mega65_h_unfreeze_from_slot(0);

        while (1) {
            VICIV.bordercol = (VICIV.bordercol + 1) & 0b1111;
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
