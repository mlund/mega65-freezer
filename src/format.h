#pragma once

#include <stdint.h>

/* Number-to-text with no hardware behind it: each writes into the caller's
 * buffer, so src/format.c compiles for the host and is tested there. */

constexpr uint8_t DECIMAL_COLUMNS = 5;

uint8_t nybl_to_screen(uint8_t v);
void format_hex(char* out, const int32_t value, const char columns);
/* Five columns, space-padded on the left; no terminator. */
void format_decimal(char* out, uint16_t v);

char* append_str(char* at, const char* text);
char* append_hex(char* at, int32_t value, uint8_t columns);
char* append_dec(char* at, uint16_t value);
char* append_dec2(char* at, uint8_t value);
