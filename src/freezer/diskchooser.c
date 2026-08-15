/*
  Disk chooser for freeze menu.

  It is displayed over the top of the normal freeze menu,
  and so we use that screen mode.

  We get our list of disknames and put them at $40000.
  As we only care about their names, and file names are
  limited to 64 characters, we can fit ~1000.
  In fact, we can only safely mount images with names <32
  characters.

  We return the disk image name or a NULL pointer if the
  selection has failed and $FFFF if the user cancels selection
  of a disk.
*/

#include "colours.h"
#include "common.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

// The directory listing is built at $40000, one 64-byte name per entry.
// DIR_ENTRY_INDEX widens before shifting, so an index past 511 cannot overflow
// 16-bit int; the shift is what the rest of this file already uses for x64.
#define DIR_NAME_BUF 0x40000L
// The listing holds at most 0xffff/64 = 1023 entries, and 1023 << 6 = 65472
// fits an unsigned 16-bit value.  The cast is what keeps it unsigned: a short
// promotes to *signed* int, which stops at 32767, so from entry 512 the shift
// would reach the sign bit.
#define DIR_ENTRY_INDEX(n) ((uint16_t)(n) << 6)

#include <mega65.h>
#include <stdio.h>
#include <string.h>

extern uint16_t slot_number;

static short file_count = 0, min_dir_entry = 0;
static short selection_number = 0;
static short display_offset = 0;

/* freezer/menu.cpp, the one C++ translation unit: the chooser's standing text
 * as screen codes, converted when it was compiled rather than on every redraw. */
const uint8_t* chooser_help_stream(void);
const uint8_t* chooser_scanning_stream(void);

// use DMA lcopy overlap trick to save space!
static uint8_t normal_row[4] = {0, SchemeText, 0, SchemeText};
static uint8_t error_row[4] = {0, SchemeError, 0, SchemeError};
static uint8_t highlight_row[4] = {
    0, SchemeSelected | AttribReverse, 0, SchemeSelected | AttribReverse};
static uint8_t dir_line_colour[4] = {0, SchemeAccent, 0, SchemeAccent};

static char disk_name_return[33];
static char old_disk_name[33];
static uint8_t old_disk_flags, old_disk_len;

/* The disk image formats the chooser can create. */
enum DiskType : uint8_t {
    DiskTypeD81 = 0,
    DiskTypeD64 = 1,
    DiskTypeD65 = 2,
    DiskTypeD71 = 3,
};
static uint8_t disk_type, current_sector, dir_track, entries, cur_row, next_sector;
static uint8_t current_side = 0;
/* Exactly 18 screen columns, deliberately without a terminator. */
static uint8_t entry_buffer[18] __attribute__((nonstring)) = "\"                 ";

/* Colour a run of cells from a two-cell pattern.  The second copy's source
 * trails its destination by one pattern, so the DMA expands those four bytes
 * over the whole run rather than needing a table as long as the row. */
static void colour_run(uint32_t at, const uint8_t* pattern, uint16_t bytes) {
    lcopy((uint32_t)pattern, at, 4);
    lcopy(at, at + 4, bytes - 4);
}

void display_error(void) {
    char* errstr;

    VICIV.bordercol = SchemeError;
    const uint8_t code = mega65_geterrorcode();
    /* The commonest failure here, and the one hyppo reports as the end of the
     * directory on hardware, so the code alone would be shown for it. */
    errstr = hyppo_file_absent(code) ? "FILE NOT FOUND" : hyppoerror_to_screen(code);
    /* No message hyppoerror_to_screen returns exceeds the 19-cell field: the
     * longest are 18, and the fallback is "ERROR CODE XX". */
    draw_text(SCREEN_CELL(21, 0), SchemeError, errstr, (uint8_t)strlen(errstr));
    /* The whole field, not just the text: a shorter message still has to clear
     * the colour of the one it replaced. */
    colour_run(COLOUR_RAM_ADDRESS + (21 * 2), error_row, 19 * 2);
}

void draw_directory_entry(uint8_t screen_row) {

    lcopy_skip((Addr28)(uint16_t)entry_buffer,
        SCREEN_ADDRESS + SCREEN_CELL(21, screen_row),
        18,
        SCREEN_CELL_BYTES);

    colour_run(
        COLOUR_RAM_ADDRESS + (screen_row * SCREEN_ROW_BYTES + (21 * 2)), dir_line_colour, 19 * 2);
}

uint8_t next_directory_entry(void) {
    uint8_t c;
    uint8_t i;
    uint8_t type = 0;

    if (disk_type == DiskTypeD81 || disk_type == DiskTypeD64) {
        // D81 || D64
        i = F011_DATA;                      // track next dir
        c = F011_DATA;                      // sector next dir
        if (next_sector == 255) {           // only first two bytes of sector count!
            if (i > 0 && c > 1 && c < 41) { // track 0 means end of dir
                next_sector = c;
            } else {
                next_sector = 254; // first two bytes of sector read
            }
        }
        type = F011_DATA; // file type
        (void)F011_DATA;  // track file
        (void)F011_DATA;  // sector file
        // now 16 char filename
        if (type) { // valid
            entry_buffer[17] = ' ';
            for (i = 1; i < 17; i++) {
                entry_buffer[i] = petscii_to_screen(F011_DATA);
            }
            for (; (entry_buffer[i] & 0xbf) == ' ' && i > 1; i--) {
                ; // this might be 0x20 or 0x60
            }
            entry_buffer[i + 1] = '"';

            // skip rest up to 32 bytes
            for (i = 0; i < 11; i++) {
                (void)F011_DATA;
            }
        } else {
            for (i = 0; i < 27; i++) {
                (void)F011_DATA;
            }
        }
    }

    return type;
}

void draw_entries(void) {

    // next_sector = 255 -> first entry of sector to be read
    // next_sector = 254 -> first already read, no valid dir pointer
    // next_sector < 41  -> next dir sector
    next_sector = 255;
    for (uint8_t i = 0; i < entries; i++) {
        if (next_directory_entry()) {
            draw_directory_entry(cur_row);
            cur_row++;
        }
        if (cur_row >= 23) {
            break;
        }
        if (i == 8) { // D81 two sectors at once, so read next pointer
            next_sector = 255;
        }
    }
}

int read_sector_with_cancel(void) {
    F011_TRACK = dir_track;
    F011_SECTOR = current_sector;
    F011_SIDE = current_side;
    while (F011_STATUS & F011_STATUS_BUSY) {
        // Exit if a key has been pressed
        if (ASCIIKEY) {
            F011_CONTROL = 0;
            return 0;
        }
    }
    F011_COMMAND = F011_CMD_READ_SECTOR;
    while (F011_STATUS & F011_STATUS_BUSY) {
        // Exit if a key has been pressed
        if (ASCIIKEY) {
            F011_CONTROL = 0;
            return 0;
        }
    }
    if (F011_STATUS & F011_STATUS_READ_ERROR) {
        return 0; // abort if the sector read failed
    }

    return 1;
}

/* The selected entry, with the padding the directory stores it with trimmed
 * off.  32 bytes into a 33-byte buffer, so the terminator always has room even
 * when every byte of the name is used.  Index 0 is never trimmed, so a caller
 * may still test it for the leading slash that marks a directory. */
static void fetch_selected_name(void) {
    lcopy(DIR_NAME_BUF + DIR_ENTRY_INDEX(selection_number), (uint32_t)disk_name_return, 32);
    for (uint8_t x = 31; x && disk_name_return[x] == ' '; x--) {
        disk_name_return[x] = 0;
    }
}

uint8_t draw_directory_contents(uint8_t drive_id) {
    uint8_t c;
    uint8_t i;
    uint8_t x;
    short skip_bytes = 0;
    short j;

    // only work on drive 0 and 1
    if (drive_id > 1) {
        return 0;
    }

    fetch_selected_name();

    // Don't draw directories
    if (disk_name_return[0] == '/') {
        return 0;
    }

    // Try to mount it, with the border marked busy while working
    VICIV.bordercol = SchemeBorderBusy;
    if (mega65_dos_attach(disk_name_return, drive_id)) {
        // Mounting the image failed
        display_error();
        return 1;
    }
    VICIV.bordercol = SchemeBorder;

    // Exit if a key has been pressed
    if (ASCIIKEY) {
        return 1;
    }

    // determine disk image type
    // d68a.6/7 -> d64 flag
    // d68b.6/7 -> d65 flag
    disk_type =
        ((SDFDC_CONTROL >> (5 + drive_id)) & 0x2) | ((SDFDC_IMAGE_TYPE >> (6 + drive_id)) & 0x1);
    switch (disk_type) {
        case DiskTypeD81:
            dir_track = 39;
            current_side = 0;
            current_sector = 1;
            skip_bytes = 4;
            break;
        case DiskTypeD64:
            dir_track = 8;
            current_side = 1;
            current_sector = 9;
            skip_bytes = 256 + 0x90;
            break;
        case DiskTypeD65:
        case DiskTypeD71:
            // write_entry
            return 1; // not supported
        default:
            break;
    }

    // Mounted disk, so now get the directory.

    // Read T40 S1 (sectors begin at 1, not 0)
    F011_CONTROL = F011_CTRL_MOTOR_LED | drive_id;
    F011_COMMAND = F011_CMD_SPINUP;

    if (!read_sector_with_cancel()) {
        goto exit_with_motor_off;
    }

    // skip start of sector until we reach the disk title
    for (j = 0; j < skip_bytes; j++) {
        (void)F011_DATA;
    }

    // Then draw title at the top of the screen
    SCREEN[21 * 2] = '"';
    for (x = 0; x < 16; x++) {
        c = F011_DATA;
        if (c >= 'A' && c <= 'Z') {
            c &= 0x1f;
        }
        SCREEN[(22 + x) * 2] = c & 0x7f;
    }
    SCREEN[38 * 2] = '"';
    // reverse for disk title
    for (i = 0; i < 18; i++) {
        lpoke(COLOUR_RAM_ADDRESS + (21 * 2) + 1 + i * 2, SchemeAccent | AttribReverse);
    }

    // user impatient?
    if (ASCIIKEY) {
        goto exit_with_motor_off;
    }

    // move to first dir entry depending on disk_type
    if (disk_type == DiskTypeD81) {
        // D81
        current_sector++;
        if (!read_sector_with_cancel()) {
            goto exit_with_motor_off;
        }
        // Skip 1st half of sector
        x = 0;
        do {
            (void)F011_DATA;
        } while (++x);
        entries = 8;
    } else { // DiskTypeD64
        current_sector++;
        if (!read_sector_with_cancel()) {
            goto exit_with_motor_off;
        }
        entries = 8;
    }
    cur_row = 1; // begin drawing on row 1 of screen
    draw_entries();
    if (next_sector >= 254) {
        goto exit_with_motor_off;
    }
    do {
        if (disk_type == DiskTypeD81) {
            current_sector++;
            entries = 16;
            skip_bytes = 0;
        } else { // DiskTypeD64
            // with D64 dir is normally 18/1, 18/4, 18/7, ...
            if (next_sector < 2 || next_sector > 19) { // illegal next sector, abort
                goto exit_with_motor_off;
            } else if (next_sector == 2 && dir_track != 8) {
                // go back to end of track 8
                dir_track = 8;
                current_side = 1;
                current_sector = 10;
                skip_bytes = 1;
            } else if (next_sector > 2) { // 2 is the next 256 bytes of this 512 byte sector!
                // we just did read 18/1 which is 8/1/10/a, after it sector 3 is 9/0/1/a
                // 9/0 has 10 512 bytes sectors, so we only need to calculate the sector
                dir_track = 9;
                current_side = 0;
                current_sector = (next_sector - 1) / 2;
                skip_bytes = (next_sector - 1) % 2;
            } else { // we are on the right track...
                dir_track = 0;
            }
        }
        if (dir_track) {
            if (!read_sector_with_cancel()) {
                goto exit_with_motor_off;
            }
        }

        if (skip_bytes) {
            for (j = 0; j < 256; j++) {
                (void)F011_DATA;
            }
        }

        // once more, then we have the 22 entries we can display
        draw_entries();
    } while (cur_row < 23 && next_sector < 254);
    // Turn floppy LED and motor back off
exit_with_motor_off:
    F011_CONTROL = 0;
    return 1;
}

void draw_disk_image_list(void) {
    unsigned addr = SCREEN_ADDRESS;
    uint8_t i;
    uint8_t x;
    uint8_t name[64];
    /* All 25 rows, where this cleared 23: the instructions below overwrite the
     * other two immediately. */
    blank_screen();
    clear_colour_ram();

    draw_fragments(chooser_help_stream());

    for (i = 0; i < 23; i++) {
        if ((display_offset + i) < file_count) {
            // Real line
            lcopy(DIR_NAME_BUF + DIR_ENTRY_INDEX(display_offset + i), (uint32_t)name, 33);

            for (x = 0; x < 33; x++) {
                if ((name[x] >= 'A' && name[x] <= 'Z') || (name[x] >= 'a' && name[x] <= 'z')) {
                    POKE(addr + (x << 1), name[x] & 0x1f);
                } else if (name[x] == '_') {
                    POKE(addr + (x << 1), 0x46);
                } else if (name[x] == '~') {
                    POKE(addr + (x << 1), 0x27); // use a single-quote to substitute for a tilde
                } else {
                    POKE(addr + (x << 1), name[x]);
                }
            }
        }
        colour_run(COLOUR_RAM_ADDRESS + (i * SCREEN_ROW_BYTES),
            (display_offset + i) == selection_number ? highlight_row : normal_row,
            80);
        addr += (40 * 2);
    }
    VICIV.bordercol = SchemeBorder;
}

void scan_directory(uint8_t drive_id) {
    uint8_t x;
    uint8_t dir;
    char* ptr;
    struct m65_dirent* dirent;

    file_count = 0;

    closeall();

    lfill(DIR_NAME_BUF, ' ', 0xffffU);
    // Add the pseudo disks
    lcopy((uint32_t)NO_DISK_DRIVE, DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), 11);
    file_count++;
    if (drive_id == 0) {
        lcopy((uint32_t)INTERNAL_DRIVE_0, DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), 17);
        file_count++;
    } else if (drive_id == 1) {
        lcopy((uint32_t)INTERNAL_DRIVE_1, DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), 16);
        file_count++;
    }
    lcopy((uint32_t)"- NEW D81 DD IMAGE -", DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), 20);
    file_count++;

    min_dir_entry = file_count;

    in_subdirectory = false;
    dir = opendir();
    dirent = readdir(dir);
    while (dirent && ((uint16_t)dirent != 0xffffU)) {

        x = (uint8_t)strlen(dirent->d_name);

        // check DIR attribute of dirent
        if (x < 32) {
            if (dirent->d_type & 0x10) {
                // if there is a .. path, then we are in a subdir
                if (!strcmp("..", dirent->d_name)) {
                    in_subdirectory = true;
                    // overwrite makedisk
                    file_count--;
                    lfill(DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), ' ', 64);
                }
                lcopy((long)&dirent->d_name[0], DIR_NAME_BUF + 1 + DIR_ENTRY_INDEX(file_count), x);
                // Put / at the start of directory names to make them obviously different
                lpoke(DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), '/');
                // Don't list "." directory pointer
                if (strcmp(".", dirent->d_name) != 0) {
                    file_count++;
                }
            } else if (x > 4) {
                ptr = &dirent->d_name[x - 4];
                if ((!strcmp(ptr, ".D81")) || (!strcmp(ptr, ".d81")) || (!strcmp(ptr, ".D64")) ||
                    (!strcmp(ptr, ".d64")) || (!strcmp(ptr, ".D65")) || (!strcmp(ptr, ".d65"))) {
                    // File is a disk image
                    lcopy((long)&dirent->d_name[0], DIR_NAME_BUF + DIR_ENTRY_INDEX(file_count), x);
                    file_count++;
                }
            }
        }

        dirent = readdir(dir);
    }
    closedir(dir);
}

char* freeze_select_disk_image(uint8_t drive_id) {
    uint8_t x;
    int idle_time = 0;
    /* Whether the idle-timeout preview below mounted a different image, so
     * cancelling has to remount whatever was there before. */
    uint8_t must_restore_disk = 0;

    drive_id = drive_id & 1;

    selection_number = 0;
    display_offset = 0;

    blank_screen();

    // save old mounted state
    mega65_dos_getprocdesc(HYPPO_PAGE_MSB);
    old_disk_flags = HYPPO_PROCDESC->d81_flags[drive_id];
    old_disk_len = HYPPO_PROCDESC->d81_namelen[drive_id];
    for (x = 0; x < 32; x++) {
        old_disk_name[x] = HYPPO_PROCDESC->d81_name[drive_id][x];
    }
    old_disk_name[old_disk_len] = 0;

    draw_fragments(chooser_scanning_stream());

    scan_directory(drive_id);

    // If we didn't find any disk images, then just return
    if (!file_count) {
        return NULL;
    }

    // Okay, we have some disk images, now get the user to pick one!
    draw_disk_image_list();
    while (1) {
        x = ASCIIKEY;
        if (x) {
            // Clear read key
            ASCIIKEY = 0;
        }

        if (!x) {
            idle_time++;
            if (idle_time == 100 && selection_number >= min_dir_entry) {
                // After sitting idle for 1 second, try mounting disk image and displaying directory
                // listing
                if (draw_directory_contents(drive_id)) {
                    must_restore_disk = 1;
                }
            }
            usleep(10000);
            continue;
        } else {
            idle_time = 0;
        }

        switch (x) {
            case KEY_LEFT_ARROW: // <- key at top left of key board
                // Go back up one directory
                mega65_dos_chdir((unsigned char*)"..");
                selection_number = 0;
                display_offset = 0;
                scan_directory(drive_id);
                draw_disk_image_list();

                break;
            case KEY_ESC:      // ESC
            case KEY_RUN_STOP: // RUN-STOP = make no change, but only if we did not mess up the
                               // drive!
                if (must_restore_disk) {
                    if (old_disk_flags & PdImgFlagsMounted) {
                        mega65_dos_attach(old_disk_name, drive_id);
                    } else {
                        mega65_dos_detach(drive_id | (old_disk_flags & PdImgFlagsNoReal));
                    }
                }
                return NULL;
            case KEY_RETURN:
            case 0x21: // Return = select this disk.
                fetch_selected_name();

                // Try to mount it, with the border marked busy while working
                VICIV.bordercol = SchemeBorderBusy;
                if (disk_name_return[0] == '/') {
                    // Its a directory
                    mega65_dos_chdir((unsigned char*)&disk_name_return[1]);
                    selection_number = 0;
                    display_offset = 0;
                    scan_directory(drive_id);
                    draw_disk_image_list();
                } else {
                    // VICIV.bordercol = 6;
                    if (selection_number == 0 || selection_number == 1) {
                        // internal or no disk
                        if (!hdos_new_attach) {
                            // old d81detach detaches both drives, so we can't use it
                            // fallback to the old method, on return copy_imageproc_to_freezeregion
                            // will handle the currentproc_image_flags
                            if (drive_id == 0) {
                                if (selection_number == 1) {
                                    lpoke(0xffd36a1L, lpeek(0xffd36a1L) | 0x01);
                                } else {
                                    lpoke(0xffd368bL, (lpeek(0xffd368bL) & 0xb8) | 0x01);
                                }
                            } else {
                                if (selection_number == 1) {
                                    lpoke(0xffd36a1L, lpeek(0xffd36a1L) | 0x04);
                                } else {
                                    lpoke(0xffd368bL, (lpeek(0xffd368bL) & 0x47) | 0x08);
                                }
                            }
                            return disk_name_return;
                        }
                        // else hdos_new_attach
                        mega65_dos_detach(
                            drive_id | (selection_number == 0 ? M65_DOS_ATTACH_NODRIVE : 0));
                        return selection_number == 0 ? SELDISK_NODISK : SELDISK_INTERNAL;
                    } else if (selection_number == 2 && !in_subdirectory) {
                        // Create and mount new empty D81 file
                        // (this is like exec()/fork(), so there is no return value

                        // give MAKEDISK the drive we are mounting the image to
                        tool_drive_id[0] = drive_id;

                        // Tell MAKEDISK if we want a D81 or a D65 image
                        tool_density[0] = disk_name_return[7] == '8' ? 0 : 1; // 0=DD, 1=HD

                        mega65_dos_exechelper("MAKEDISK.M65");
                        // we never return to here...
                    } else {
                        if (mega65_dos_attach(disk_name_return, drive_id)) {
                            // Mounting the image failed
                            display_error();
                            // Unmount
                            mega65_dos_detach(drive_id);
                            break;
                        }
                    }

                    // only do this if an image was mounted
                    if (selection_number >= min_dir_entry) {
                        // Mount succeeded, now seek to track 0 to make sure DOS
                        // knows where we are, and to make sure the drive head is
                        // sitting properly.
                        F011_CONTROL = F011_CTRL_MOTOR_LED | drive_id;
                        F011_COMMAND = F011_CMD_SPINUP;

                        while (!(F011_STATUS & F011_STATUS_AT_TRACK0)) {
                            F011_COMMAND = F011_CMD_STEP;
                            usleep(7000);
                        }
                        // Now check the contents of $D084 to find out the most recently
                        // requested track, and seek the head to that track.
                        x = freeze_io_peek(0x3084); // Get last requested track by frozen programme
                        while (x) {
                            F011_COMMAND = F011_CMD_SEEK;
                            while (F011_STATUS & F011_STATUS_BUSY) {
                            }
                            x--;
                        }
                        F011_CONTROL = 0; // motor and led off
                    }

                    // Mounted ok, so return this image
                    return disk_name_return;
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

        draw_disk_image_list();
    }

    return NULL;
}
