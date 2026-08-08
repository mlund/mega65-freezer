/* The `B` command: browse and edit the bits of frozen memory, with the I/O
 * registers named.  bitedit_table.c decides what a row says; this file puts it
 * on screen and reads the keyboard. */

#include "bitedit.h"

#include "bitedit_table.h"
#include "blockmove.h"
#include "colours.h"
#include "console.h"
#include "dma.h"
#include "format.h"
#include "helper.h"
#include "mega65_regs.h"
#include "screen.h"
#include "trace.h"

#include <string.h>

/* The editor takes the display while it runs, so the count is whatever fits
 * above the footer under the heading.  A scrolling console has nothing to offer
 * a table that is repainted in place, and owning the screen means the rows sit
 * at a known address instead of wherever printing them happened to land. */
constexpr uint8_t EDIT_ROWS = 14;
constexpr uint16_t WINDOW_TOP = SCREEN_ADDRESS + SCREEN_ROW_BYTES;

/* The description block, fenced by a rule above and below.  Seven lines is
 * enough that no register's own text plus its longest bit's ever overflows, so
 * the two share the space rather than each taking a fixed cut. */
constexpr uint8_t INFO_ROWS = 7;
constexpr uint8_t INFO_TAG_WIDTH = 7;
constexpr uint8_t RULE_ROW = EDIT_ROWS + 1;
constexpr uint8_t INFO_ROW = RULE_ROW + 1;
constexpr uint8_t FOOTER_RULE_ROW = INFO_ROW + INFO_ROWS;

/* Where the database is parked once loaded.  Nothing MONITOR links touches
 * anything above $1F800, and the frozen program's own $40000 lives in the
 * freeze slot on the card, so this is scratch. */
constexpr Addr28 IOMAP_ADDRESS = 0x40000L;
static bool iomap_ready;
static bool iomap_tried;

static char row_text[BITEDIT_ROW_WIDTH];
static char row_colour[BITEDIT_ROW_WIDTH];

/* bitedit_table.c asks for the database through this and nothing else, which is
 * what lets the host test hand it a buffer instead of a card.  lpeek is a flat
 * load rather than a DMA job, so a byte at a time is the right granularity and
 * the decoder needs no cache of its own. */
uint8_t iomap_byte(uint16_t offset) {
    return iomap_ready ? lpeek(IOMAP_ADDRESS + offset) : 0;
}

static void paint_row(uint8_t row, uint8_t value, uint8_t cursor_cell) {
    const uint16_t screen = WINDOW_TOP + row * SCREEN_ROW_BYTES;

    memset(row_colour, SchemeTextDim, BITEDIT_ROW_WIDTH);
    memset(row_colour, SchemeAddress, BITEDIT_ADDRESS_WIDTH);

    /* Brightness is the whole set/clear signal, so the pair has to be the
     * furthest apart the palette offers rather than merely different: the
     * classic scheme's text_dim and text_bright are two adjacent C64 greys,
     * 45 apart in luminance where these two are 164.  Walking a mask rather
     * than shifting by the bit number keeps the shift constant, which this
     * target lowers to a loop otherwise. */
    char* cell = &row_colour[BITEDIT_BIT_COLUMN + 7 * BITEDIT_BIT_STRIDE];
    uint8_t mask = 1;
    for (uint8_t bit = 0; bit < 8; bit++) {
        const char shade = cursor_cell == bit ? SchemeHighlight
            : (value & mask)                  ? SchemeText
                                              : SchemeUnselected;
        memset(cell, shade, BITEDIT_BIT_WIDTH);
        cell -= BITEDIT_BIT_STRIDE;
        mask = (uint8_t)(mask << 1);
    }
    row_colour[BITEDIT_VALUE_COLUMN] = row_colour[BITEDIT_VALUE_COLUMN + 1] =
        cursor_cell == BITEDIT_CELL_VALUE ? SchemeHighlight : SchemeValue;

    lcopy((long)row_text, screen, BITEDIT_ROW_WIDTH);
    lcopy((long)row_colour, COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + screen, BITEDIT_ROW_WIDTH);
}

/* A frozen region ends wherever the hypervisor's list says, so an address just
 * past one reads as unmapped.  Probing costs nothing: the offset lookup fails
 * before any sector is touched. */
static bool readable(uint32_t address) {
    uint8_t value;
    return disasm_read_byte(address, &value);
}

static void draw_info(uint32_t address, const RegisterInfo* info, uint8_t cursor_cell);

static void draw_window(uint32_t top, uint8_t cursor_row, uint8_t cursor_cell) {
    uint8_t previous_chip = BITEDIT_NO_CHIP;
    uint16_t scan = 0;
    RegisterInfo cursor_info;

    memset(&cursor_info, 0, sizeof(cursor_info));
    for (uint8_t row = 0; row < EDIT_ROWS; row++) {
        const uint32_t address = top + row;
        const uint8_t cursor = row == cursor_row ? cursor_cell : BITEDIT_CELL_COUNT;
        uint8_t value;

        /* A window opened near the end of a region runs off it.  Blank those
         * rows rather than refusing to draw: the gap is what tells the user
         * where the region stops. */
        if (!disasm_read_byte(address, &value)) {
            lfill((long)row_text, ' ', BITEDIT_ROW_WIDTH);
            paint_row(row, 0, BITEDIT_CELL_COUNT);
            continue;
        }
        RegisterInfo info;
        bitedit_lookup(bitedit_io_address(address), &info, &scan);
        previous_chip = bitedit_render(row_text, address, value, &info, previous_chip);
        paint_row(row, value, cursor);
        if (row == cursor_row) {
            cursor_info = info;
        }
    }
    draw_info(top + cursor_row, &cursor_info, cursor_cell);
}

/* Load the database once per monitor session.  A second B costs nothing, and a
 * missing file is not an error: the editor simply has no names for anything,
 * which is the same thing it shows for an address the database never covered. */
static void load_database(void) {
    if (iomap_tried) {
        return;
    }
    iomap_tried = true;

    /* The loader resolves against hyppo's working directory and MONITOR, unlike
     * the freezer, has no root check of its own. */
    mega65_dos_cdroot();
    iomap_ready = read_file_from_sdcard("IOMAP.M65", IOMAP_ADDRESS) == 0;
    if (iomap_ready) {
        iomap_ready = bitedit_open();
    }
    TRACE(iomap_ready ? "IOMAP LOADED" : "IOMAP MISSING");
}

/* Returns the row after the last one used, because the register and bit fields
 * share the block rather than each taking a fixed cut of it. */
static uint8_t put_info(
    uint8_t row, const char* tag, uint8_t tag_length, const char* body, uint16_t length) {
    char line[BITEDIT_ROW_WIDTH];
    uint16_t at = 0;

    while (row < FOOTER_RULE_ROW) {
        const bool first = at == 0;
        memset(line, ' ', BITEDIT_ROW_WIDTH);
        if (first) {
            memcpy(line, tag, tag_length);
        }
        const uint8_t take = bitedit_wrap(body, length, at, BITEDIT_ROW_WIDTH - INFO_TAG_WIDTH);
        memcpy(&line[INFO_TAG_WIDTH], &body[at], take);
        at = (uint16_t)(at + take);

        const uint16_t screen = WINDOW_TOP + (row - 1) * SCREEN_ROW_BYTES;
        lcopy((long)line, screen, BITEDIT_ROW_WIDTH);
        lfill(COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + screen, SchemeTextDim, BITEDIT_ROW_WIDTH);
        if (first) {
            lfill(COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + screen, SchemeText, tag_length);
        }
        row++;
        if (at >= length) {
            break;
        }
    }
    return row;
}

/* `$D011` or `$D011.5`, in screen codes.  Always available and always true,
 * which is why it stands in wherever a description is missing. */
static uint8_t reference(char* tag, uint16_t io_address, int8_t bit) {
    tag[0] = '$';
    format_hex(&tag[1], io_address, 4);
    if (bit < 0) {
        return 5;
    }
    tag[5] = '.';
    tag[6] = (char)(0x30 + bit);
    return 7;
}

/* What the register line says when iomap.txt has no prose for it: the chip and
 * the register's own mnemonic, which is still more than an empty line. */
static uint16_t register_fallback(const RegisterInfo* info, char* body) {
    uint16_t length = 0;
    if (info->field_count || info->name) {
        length = bitedit_string(bitedit_chip_name(info->chip), body, BITEDIT_NAME_WIDTH);
    }
    if (info->name) {
        body[length++] = ' ';
        body[length++] = ' ';
        length = (uint8_t)(length + bitedit_string(info->name, &body[length], BITEDIT_NAME_WIDTH));
    }
    return length;
}

static void draw_info(uint32_t address, const RegisterInfo* info, uint8_t cursor_cell) {
    char tag[INFO_TAG_WIDTH];
    char body[320];
    const uint16_t io_address = bitedit_io_address(address);
    uint8_t row = INFO_ROW;

    lfill(WINDOW_TOP + (INFO_ROW - 1) * SCREEN_ROW_BYTES, ' ', INFO_ROWS * SCREEN_ROW_BYTES);

    /* The register line first, then whatever is left goes to the bit line.
     * Sharing beats a fixed split: register text runs to 156 characters and bit
     * text to 318, so any fixed division truncates what the other half had room
     * for. */
    if (io_address) {
        const uint8_t tag_length = reference(tag, io_address, -1);
        uint16_t length = bitedit_string(info->text, body, (uint16_t)(sizeof(body) - 1));
        if (!length) {
            length = register_fallback(info, body);
        }
        row = put_info(row, tag, tag_length, body, length);
    }

    if (cursor_cell == BITEDIT_CELL_VALUE) {
        return;
    }
    const uint8_t field = info->bit_field[cursor_cell];
    if (info->field_count && field < info->field_count) {
        const uint8_t tag_length =
            (uint8_t)bitedit_string(info->field_name[field], tag, INFO_TAG_WIDTH);
        const uint16_t length =
            bitedit_string(info->field_text[field], body, (uint16_t)(sizeof(body) - 1));
        put_info(row, tag, tag_length, body, length);
    } else if (io_address) {
        /* No name and no prose: say which bit the cursor is on, which is always
         * true and is what a reader would look up. */
        const uint8_t tag_length = reference(tag, io_address, (int8_t)cursor_cell);
        put_info(row, tag, tag_length, body, 0);
    }
}

static void rule(uint8_t row) {
    /* A horizontal line in the ROM charset, so the block reads as its own
     * region rather than as more table. */
    constexpr uint8_t GLYPH_RULE = 0x40;
    const uint16_t screen = WINDOW_TOP + (row - 1) * SCREEN_ROW_BYTES;
    lfill(screen, GLYPH_RULE, BITEDIT_ROW_WIDTH);
    lfill(COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + screen, SchemeTextDim, BITEDIT_ROW_WIDTH);
}

/* Clear everything above the footer and put the heading on the top line.  The
 * footer is left alone: display_footer() owns row 24. */
static void open_window(void) {
    constexpr uint16_t USED = (FOOTER_RULE_ROW + 1) * SCREEN_ROW_BYTES;
    lfill(SCREEN_ADDRESS, ' ', USED);
    lfill(COLOUR_RAM_ADDRESS, SchemeTextDim, USED);
    bitedit_header(row_text);
    lcopy((long)row_text, SCREEN_ADDRESS, BITEDIT_ROW_WIDTH);
    rule(RULE_ROW);
    rule(FOOTER_RULE_ROW);
}

/* Every write, on the hypervisor serial channel.  The address and value are
 * what the emulator test asserts on: they are plain text, so they survive a
 * screen the test cannot read the colours of, and they show each write rather
 * than only the byte left behind at the end. */
static bool write_byte(uint32_t address, uint8_t value) {
#ifdef FREEZER_TRACE
    /* Plain ASCII, not format.h's helpers: those emit screen codes, which reach
     * the serial channel as control characters rather than digits. */
    static const char DIGIT[] = "0123456789ABCDEF";
    char line[20] = "BITEDIT ";
    /* Seven nibbles, most significant first: a frozen address is 28 bits. */
    for (uint8_t i = 0; i < 7; i++) {
        line[8 + i] = DIGIT[(address >> ((6 - i) * 4)) & 0xF];
    }
    line[15] = ' ';
    line[16] = DIGIT[value >> 4];
    line[17] = DIGIT[value & 0xF];
    line[18] = 0;
    debug_msg(line);
#endif
    return block_write_byte(address, value);
}

bool edit_bits(void) {
    uint32_t top = mon_address;
    uint8_t cursor_row = 0;
    uint8_t cursor_cell = 7;

    /* The only failure worth reporting: the address the user asked for is not
     * in the slot at all.  Running off the end of a region while scrolling is
     * an edge to stop at, not an error. */
    if (!readable(top)) {
        return false;
    }

    load_database();
    open_window();
    draw_window(top, cursor_row, cursor_cell);
    while (ASCIIKEY) {
        ASCIIKEY = 0;
    }

    for (;;) {
        const uint8_t key = ASCIIKEY;
        if (!key) {
            continue;
        }
        ASCIIKEY = 0;

        const uint32_t address = top + cursor_row;
        uint8_t value = 0;
        const bool present = disasm_read_byte(address, &value);
        uint8_t edited = value;

        switch (key) {
            case KEY_CURSOR_LEFT:
                cursor_cell = bitedit_cursor_left(cursor_cell);
                break;
            case KEY_CURSOR_RIGHT:
                cursor_cell = bitedit_cursor_right(cursor_cell);
                break;
            /* At an edge the window follows rather than the cursor, so
             * scrolling needs no key of its own -- but only as far as the
             * region reaches, so holding a cursor key stops there instead of
             * ending the command. */
            case KEY_CURSOR_UP:
                if (cursor_row) {
                    cursor_row--;
                } else if (readable(top - 1)) {
                    top--;
                }
                break;
            case KEY_CURSOR_DOWN:
                /* The row below has to be readable either way: a window opened
                 * near the end of a region has blank rows after it, and the
                 * cursor is invisible on one of those. */
                if (cursor_row + 1 < EDIT_ROWS) {
                    if (readable(top + cursor_row + 1)) {
                        cursor_row++;
                    }
                } else if (readable(top + EDIT_ROWS)) {
                    top++;
                }
                break;

            case ' ':
            case KEY_RETURN:
                if (cursor_cell != BITEDIT_CELL_VALUE) {
                    edited = bitedit_toggle(value, cursor_cell);
                }
                break;
            case '+':
                edited = (uint8_t)(value + 1);
                break;
            case '-':
                edited = (uint8_t)(value - 1);
                break;

            case KEY_RUN_STOP:
                mon_address = top;
                /* The table was drawn over the console without moving its write
                 * point, so the next prompt would land inside it. */
                setup_screen();
                return true;

            default:
                if (cursor_cell == BITEDIT_CELL_VALUE) {
                    if (key >= '0' && key <= '9') {
                        edited = bitedit_hex_digit(value, (uint8_t)(key - '0'));
                    } else if (key >= 'a' && key <= 'f') {
                        edited = bitedit_hex_digit(value, (uint8_t)(key - 'a' + 10));
                    } else if (key >= 'A' && key <= 'F') {
                        edited = bitedit_hex_digit(value, (uint8_t)(key - 'A' + 10));
                    }
                }
                break;
        }

        if (present && edited != value && !write_byte(address, edited)) {
            return false;
        }
        draw_window(top, cursor_row, cursor_cell);
    }
}
