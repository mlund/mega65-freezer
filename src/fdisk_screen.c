#include "fdisk_screen.h"

#include "fdisk_memory.h"
#include "freezer_common.h"

long screen_line_address = SCREEN_ADDRESS;
char screen_column = 0;

/* Writes eight hex digits to a buffer. */
static void hex_to_buf(char* out, const long value) {
    out[0] = nybl_to_screen(value >> 28);
    out[1] = nybl_to_screen(value >> 24);
    out[2] = nybl_to_screen(value >> 20);
    out[3] = nybl_to_screen(value >> 16);
    out[4] = nybl_to_screen(value >> 12);
    out[5] = nybl_to_screen(value >> 8);
    out[6] = nybl_to_screen(value >> 4);
    out[7] = nybl_to_screen(value >> 0);
}

void screen_hex(unsigned int addr, long value) {
    char i;
    char dec[8];
    hex_to_buf(dec, value);
    for (i = 0; i < 8; i++)
        POKE(addr + i, dec[i]);
}

/* Writes the low `columns` hex digits of value to out; the higher digits are
 * dropped.  out is a plain buffer, so store through the pointer -- passing an
 * address as an integer and rebuilding a pointer from it inside the callee
 * leaves the compiler unable to connect the stores to the caller's object. */
void format_hex(char* out, const long value, const char columns) {
    char i;
    char dec[8];
    hex_to_buf(dec, value);

    for (i = 0; i < columns; i++)
        out[i] = dec[i + 8 - columns];
}

static const unsigned char screen_decimal_digits[16][5] = {{0, 0, 0, 0, 1},
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

void screen_decimal(unsigned int addr, unsigned int v) {
    // XXX - We should do this off-screen and copy into place later, to avoid glitching
    // on display.
    unsigned char digits[5];
    unsigned char ii, j, carry, temp;

    // Start with all zeros
    for (ii = 0; ii < 5; ii++)
        digits[ii] = 0;

    // Add power of two strings for all non-zero bits in value.
    // XXX - We should use BCD mode to do this more efficiently
    for (ii = 0; ii < 16; ii++) {
        if (v & 1) {
            carry = 0;
            for (j = 4; j < 128; j--) {
                temp = digits[j] + screen_decimal_digits[ii][j] + carry;
                if (temp > 9) {
                    temp -= 10;
                    carry = 1;
                } else
                    carry = 0;
                digits[j] = temp;
            }
        }
        v = v >> 1;
    }

    // Now convert to ascii digits
    for (j = 0; j < 5; j++)
        digits[j] = digits[j] | '0';

    // and shift out leading zeros
    for (j = 0; j < 4; j++) {
        if (digits[0] != '0')
            break;
        digits[0] = digits[1];
        digits[1] = digits[2];
        digits[2] = digits[3];
        digits[3] = digits[4];
        digits[4] = ' ';
    }

    // Copy to screen
    for (j = 0; j < 5; j++)
        POKE(addr + j, digits[j]);
}
