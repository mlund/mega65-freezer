#include "screen.h"

#include "colours.h"
#include "common.h"
#include "dma.h"
#include "mega65_regs.h"

uint16_t screen_line_address = SCREEN_ADDRESS;
char screen_column = 0;

/* Writes eight hex digits to a buffer. */
static void hex_to_buf(char* out, const long value) {
    out[0] = nybl_to_screen((uint8_t)(value >> 28));
    out[1] = nybl_to_screen((uint8_t)(value >> 24));
    out[2] = nybl_to_screen((uint8_t)(value >> 20));
    out[3] = nybl_to_screen((uint8_t)(value >> 16));
    out[4] = nybl_to_screen((uint8_t)(value >> 12));
    out[5] = nybl_to_screen((uint8_t)(value >> 8));
    out[6] = nybl_to_screen((uint8_t)(value >> 4));
    out[7] = nybl_to_screen((uint8_t)(value >> 0));
}

void screen_hex(uint16_t addr, long value) {
    char dec[8];
    hex_to_buf(dec, value);
    for (char i = 0; i < 8; i++) {
        POKE(addr + i, dec[i]);
    }
}

/* Writes the low `columns` digits.  out is a plain buffer, so take a pointer:
 * an address passed as an integer hides the stores from the compiler. */
void format_hex(char* out, const long value, const char columns) {
    char dec[8];
    hex_to_buf(dec, value);

    for (char i = 0; i < columns; i++) {
        out[i] = dec[i + 8 - columns];
    }
}

static const unsigned char SCREEN_DECIMAL_DIGITS[16][5] = {{0, 0, 0, 0, 1},
    {0, 0, 0, 0, 2},
    {0, 0, 0, 0, 4},
    {0, 0, 0, 0, 8},
    {0, 0, 0, 1, 6},
    {0, 0, 0, 3, 2},
    {0, 0, 0, 6, 4},
    {0, 0, 1, 2, 8},
    {0, 0, 2, 5, 6},
    {0, 0, 5, 1, 2},
    {0, 1, 0, 2, 4},
    {0, 2, 0, 4, 8},
    {0, 4, 0, 9, 6},
    {0, 8, 1, 9, 2},
    {1, 6, 3, 8, 4},
    {3, 2, 7, 6, 8}};

static void format_decimal(char* out, uint16_t v) {
    // XXX - We should do this off-screen and copy into place later, to avoid glitching
    // on display.
    unsigned char digits[5];
    unsigned char ii;
    unsigned char j;
    unsigned char carry;
    unsigned char temp;

    // Start with all zeros
    for (ii = 0; ii < 5; ii++) {
        digits[ii] = 0;
    }

    // Add power of two strings for all non-zero bits in value.
    // XXX - We should use BCD mode to do this more efficiently
    for (ii = 0; ii < 16; ii++) {
        if (v & 1) {
            carry = 0;
            for (j = 4; j < 128; j--) {
                temp = digits[j] + SCREEN_DECIMAL_DIGITS[ii][j] + carry;
                if (temp > 9) {
                    temp -= 10;
                    carry = 1;
                } else {
                    carry = 0;
                }
                digits[j] = temp;
            }
        }
        v = v >> 1;
    }

    // Now convert to ascii digits
    for (j = 0; j < 5; j++) {
        digits[j] = digits[j] | '0';
    }

    // and shift out leading zeros
    for (j = 0; j < 4; j++) {
        if (digits[0] != '0') {
            break;
        }
        digits[0] = digits[1];
        digits[1] = digits[2];
        digits[2] = digits[3];
        digits[3] = digits[4];
        digits[4] = ' ';
    }

    for (j = 0; j < 5; j++) {
        out[j] = (char)digits[j];
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
void draw_decimal(uint8_t x, uint8_t y, uint8_t colour, uint16_t v) {
    char digits[DECIMAL_COLUMNS];
    format_decimal(digits, v);
    draw_text(x, y, colour, digits, DECIMAL_COLUMNS);
}

void draw_fragment(uint8_t x, uint8_t y, uint8_t colour, const uint8_t* codes, uint8_t length) {
    const uint16_t cell = (uint16_t)(y * SCREEN_ROW_BYTES + x * SCREEN_CELL_BYTES);

    lcopy_skip((Addr28)(uint16_t)codes, SCREEN_ADDRESS + cell, length, SCREEN_CELL_BYTES);
    lfill_skip(
        COLOUR_RAM_ADDRESS + cell + (SCREEN_CELL_BYTES - 1), colour, length, SCREEN_CELL_BYTES);
}

/* Screen RAM is directly addressable, so the characters go out with POKE; the
 * colour plane is above 64K and needs the DMAgic, but a run is one colour, so
 * that is a single strided fill rather than a job per character. */
void draw_text(uint8_t x, uint8_t y, uint8_t colour, const char* text, uint8_t length) {
    uint16_t cell = (uint16_t)(y * SCREEN_ROW_BYTES + x * SCREEN_CELL_BYTES);

    lfill_skip(
        COLOUR_RAM_ADDRESS + cell + (SCREEN_CELL_BYTES - 1), colour, length, SCREEN_CELL_BYTES);

    for (uint8_t i = 0; i < length; i++) {
        char c = text[i];
        if ((c >= 'A') && (c <= 'Z')) {
            c -= 0x40;
        } else if ((c >= 'a') && (c <= 'z')) {
            c -= 0x20;
        }
        POKE(SCREEN_ADDRESS + cell, (uint8_t)c);
        cell += SCREEN_CELL_BYTES;
    }
}

void draw_fragments(const uint8_t* stream) {
    uint8_t length;

    while ((length = stream[0]) != 0) {
        draw_fragment(stream[1], stream[2], stream[3], stream + 4, length);
        stream += 4 + length;
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

/* Super-Extended Attribute Mode reads the high nibble as attributes, so the
 * fill has to stay a plain colour. */
void clear_colour_ram(void) {
    lfill(COLOUR_RAM_ADDRESS, SchemeText, SCREEN_BYTES);
}
