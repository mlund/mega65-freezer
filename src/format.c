/* Turning numbers into text.  Nothing here touches a register: every function
 * writes into a buffer the caller owns, which is what lets
 * test/verify_format.py compile this file for the host and check it against
 * Python's own formatting. */

#include "format.h"

#include <stdbool.h>

/* The low nibble as one hex digit, in screen codes rather than ASCII: the
 * digits sit at $30 as usual, but A-F are $01-$06. */
uint8_t nybl_to_screen(uint8_t v) {
    v &= 0xf;
    if (v < 0xa) {
        return 0x30 + v;
    }
    return v - 0x9;
}

/* Always eight digits; format_hex() takes the tail it wants. */
static void hex_to_buf(char* out, const int32_t value) {
    out[0] = nybl_to_screen((uint8_t)(value >> 28));
    out[1] = nybl_to_screen((uint8_t)(value >> 24));
    out[2] = nybl_to_screen((uint8_t)(value >> 20));
    out[3] = nybl_to_screen((uint8_t)(value >> 16));
    out[4] = nybl_to_screen((uint8_t)(value >> 12));
    out[5] = nybl_to_screen((uint8_t)(value >> 8));
    out[6] = nybl_to_screen((uint8_t)(value >> 4));
    out[7] = nybl_to_screen((uint8_t)(value >> 0));
}

/* Writes the low `columns` digits.  Takes a pointer rather than an address as
 * an integer, which would hide the stores from the compiler. */
void format_hex(char* out, const int32_t value, const char columns) {
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
    uint8_t written = 0;
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
        /* Left-aligned: the digits start at the field's first column and the
         * padding follows, so a value sits against the label that names it. */
        if (!leading || i == DECIMAL_COLUMNS - 1) {
            out[written++] = (char)('0' + d);
        }
    }
    while (written < DECIMAL_COLUMNS) {
        out[written++] = ' ';
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
char* append_hex(char* at, int32_t value, uint8_t columns) {
    format_hex(at, value, (char)columns);
    return at + columns;
}

char* append_dec(char* at, uint16_t value) {
    /* Taken from the top by subtraction rather than divided down from the
     * bottom.  A general 16-bit divide is a library routine of its own, linked
     * whole for this one caller; four subtractions against a table of powers
     * are a handful of instructions and no call.  Slower per digit, which
     * costs nothing: numbers are formatted to be looked at. */
    static const uint16_t POWER[] = {10000, 1000, 100, 10};
    bool any = false;
    for (uint8_t i = 0; i < sizeof POWER / sizeof *POWER; i++) {
        uint8_t digit = 0;
        while (value >= POWER[i]) {
            value = (uint16_t)(value - POWER[i]);
            digit++;
        }
        /* Leading zeros are not digits, but a zero after the first one is. */
        if (digit || any) {
            *at++ = (char)('0' + digit);
            any = true;
        }
    }
    *at++ = (char)('0' + value);
    return at;
}

/* Exactly two digits, as a date wants. */
char* append_dec2(char* at, uint8_t value) {
    *at++ = (char)('0' + value / 10);
    *at++ = (char)('0' + value % 10);
    return at;
}

uint32_t parse_dec(const char* text) {
    uint32_t value = 0;
    for (; *text >= '0' && *text <= '9'; text++) {
        value = value * 10 + (uint8_t)(*text - '0');
    }
    return value;
}
