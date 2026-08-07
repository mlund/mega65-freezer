/* Turning numbers into text.  Nothing here touches a register: every function
 * writes into a buffer the caller owns, which is what lets
 * test/verify_format.py compile this file for the host and check it against
 * Python's own formatting. */

#include "format.h"

/*
 * uint8_t nybl_to_screen(uint8_t v)
 *
 * converts the lower 4 bits of a byte to a screen code
 * hexadecimal number digit.
 */
uint8_t nybl_to_screen(uint8_t v) {
    v &= 0xf;
    if (v < 0xa) {
        return 0x30 + v;
    }
    return v - 0x9;
}

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

/* Writes the low `columns` digits.  out is a plain buffer, so take a pointer:
 * an address passed as an integer hides the stores from the compiler. */
void format_hex(char* out, const long value, const char columns) {
    char dec[8];
    hex_to_buf(dec, value);

    for (char i = 0; i < columns; i++) {
        out[i] = dec[i + 8 - columns];
    }
}

static const uint16_t DECIMAL_POWERS[DECIMAL_COLUMNS] = {10000, 1000, 100, 10, 1};

/* Repeated subtraction, most significant power first: five compares and at most
 * nine subtractions each, against a ten-byte table. */
void format_decimal(char* out, uint16_t v) {
    uint8_t leading = 1;
    for (uint8_t i = 0; i < DECIMAL_COLUMNS; i++) {
        uint8_t d = 0;
        while (v >= DECIMAL_POWERS[i]) {
            v -= DECIMAL_POWERS[i];
            d++;
        }
        if (d) {
            leading = 0;
        }
        out[i] = (leading && i < DECIMAL_COLUMNS - 1) ? ' ' : (char)('0' + d);
    }
}

char* append_str(char* at, const char* text) {
    while (*text) {
        *at++ = *text++;
    }
    return at;
}

/* format_hex() writes `columns` digits and no terminator, which is what lets
 * these chain from a cursor. */
char* append_hex(char* at, long value, uint8_t columns) {
    format_hex(at, value, (char)columns);
    return at + columns;
}

char* append_dec(char* at, uint16_t value) {
    char digits[DECIMAL_COLUMNS];
    uint8_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) {
        *at++ = digits[--n];
    }
    return at;
}

/* Exactly two digits, as a date wants. */
char* append_dec2(char* at, uint8_t value) {
    *at++ = (char)('0' + value / 10);
    *at++ = (char)('0' + value % 10);
    return at;
}
