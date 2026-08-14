#include "screen.h"

#include "colours.h"
#include "common.h"
#include "dma.h"
#include "format.h"
#include "mega65_regs.h"
#include "screencode.h"

uint16_t screen_line_address = SCREEN_ADDRESS;
char screen_column = 0;

void screen_hex(uint16_t addr, int32_t value) {
    char dec[8];
    format_hex(dec, value, 8);
    for (char i = 0; i < 8; i++) {
        POKE(addr + i, dec[i]);
    }
}

/* Five columns, space-padded on the left.  The monitor's footer is 8-bit text,
 * so its cells are consecutive bytes and POKE reaches them directly. */
void screen_decimal(uint16_t addr, uint16_t v) {
    char digits[DECIMAL_COLUMNS];
    format_decimal(digits, v);
    for (uint8_t i = 0; i < DECIMAL_COLUMNS; i++) {
        POKE(addr + i, (uint8_t)digits[i]);
    }
}

/* The same value placed by cell rather than by address, for the 16-bit text
 * mode where a cell is two bytes. */
void draw_decimal(uint16_t cell, uint8_t colour, uint16_t v) {
    char digits[DECIMAL_COLUMNS];
    format_decimal(digits, v);
    draw_text(cell, colour, digits, DECIMAL_COLUMNS);
}

void draw_fragment(uint16_t cell, uint8_t colour, const uint8_t* codes, uint8_t length) {
    lcopy_skip((Addr28)(uint16_t)codes, SCREEN_ADDRESS + cell, length, SCREEN_CELL_BYTES);
    lfill_skip(
        COLOUR_RAM_ADDRESS + cell + (SCREEN_CELL_BYTES - 1), colour, length, SCREEN_CELL_BYTES);
}

/* Screen RAM is directly addressable, so the characters go out with POKE; the
 * colour plane is above 64K and needs the DMAgic, but a run is one colour, so
 * that is a single strided fill rather than a job per character. */
void draw_text(uint16_t cell, uint8_t colour, const char* text, uint8_t length) {
    lfill_skip(
        COLOUR_RAM_ADDRESS + cell + (SCREEN_CELL_BYTES - 1), colour, length, SCREEN_CELL_BYTES);

    for (uint8_t i = 0; i < length; i++) {
        char c = text[i];
        if ((c >= 'A') && (c <= 'Z')) {
            c -= 0x40;
        } else if ((c >= 'a') && (c <= 'z')) {
            c -= 0x20;
        }
        SCREEN[cell] = (uint8_t)c;
        cell += SCREEN_CELL_BYTES;
    }
}

/* A table cell: ASCII in, truncated or space-padded to the column's width, so
 * a shorter value cannot leave the previous one's tail behind it.  The padding
 * is a fill because it converts nothing -- only the text needs the per-byte
 * loop draw_text() also uses. */
void draw_field(uint16_t cell, uint8_t colour, const char* text, uint8_t width) {
    lfill_skip(
        COLOUR_RAM_ADDRESS + cell + (SCREEN_CELL_BYTES - 1), colour, width, SCREEN_CELL_BYTES);

    uint8_t length = 0;
    while (length < width && text[length]) {
        SCREEN[cell] = ascii_to_screen((uint8_t)text[length]);
        cell += SCREEN_CELL_BYTES;
        length++;
    }
    lfill_skip(SCREEN_ADDRESS + cell, ' ', width - length, SCREEN_CELL_BYTES);
}

void draw_fragments(const uint8_t* stream) {
    uint8_t length;

    while ((length = stream[0]) != 0) {
        draw_fragment(
            (uint16_t)(stream[1] | ((uint16_t)stream[2] << 8)), stream[3], stream + 4, length);
        stream += 4 + length;
    }
}

void draw_rule(uint16_t cell, uint8_t width) {
    lfill_skip(SCREEN_ADDRESS + cell, MENU_RULE_GLYPH, width, SCREEN_CELL_BYTES);
    lfill_skip(COLOUR_RAM_ADDRESS + cell + (SCREEN_CELL_BYTES - 1),
        SchemeTextDim,
        width,
        SCREEN_CELL_BYTES);
}

/* A cell is one byte or two, so the space goes out strided rather than seeded
 * and copied: the 16-bit mode holds a character code across the pair, whose
 * high byte has to be cleared as well. */
void blank_screen(void) {
    constexpr uint16_t CELLS = SCREEN_BYTES / SCREEN_CELL_BYTES;

    lfill_skip(SCREEN_ADDRESS, 0x20, CELLS, SCREEN_CELL_BYTES);
    if (SCREEN_CELL_BYTES > 1) {
        lfill_skip(SCREEN_ADDRESS + 1, 0x00, CELLS, SCREEN_CELL_BYTES);
    }
}

void setup_menu_screen_base(void) {
    /* Written before the base registers below, so the hot-register
     * recalculation that lands is the one the second write triggers. */
    VICIV.addr = VIC4_ADDR_UPPERCASE;

    VICIV.sdbdrwd_lsb = VIC4_SIDE_BORDER_WIDTH;
    VICIV.sdbdrwd_msb = VIC4_BORDER_MSB_HOTREG;
    VICIV.spr_ena = 0x00;

    VICIV.addr =
        (((CHARSET_ADDRESS - 0x8000U) >> 11) << 1) + (((SCREEN_ADDRESS - 0x8000U) >> 10) << 4);
    CIA2.pra = (CIA2.pra & CIA2_VIC_BANK_MASK) | CIA2_VIC_BANK_8000;
}

/* Takes the cell index, not the 28-bit address: two argument bytes ride in A and
 * X, and each byte past those costs marshalling into an imaginary register at
 * every call site.  Widening the signature back to an address would pay that
 * everywhere; casting at the call site would not, since what the site passes is
 * what costs, not where the arithmetic widens.  Size for speed: a jsr per cell. */
void colour_poke(uint16_t cell, uint8_t value) {
    lpoke(COLOUR_RAM_ADDRESS + cell, value);
}

/* Super-Extended Attribute Mode reads the high nibble as attributes, so the
 * fill has to stay a plain colour. */
void clear_colour_ram(void) {
    lfill(COLOUR_RAM_ADDRESS, SchemeText, SCREEN_BYTES);
}
