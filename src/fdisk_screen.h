#pragma once

#include <stdint.h>

#define SCREEN_ADDRESS (0xB800U)
// Use default char set, not ASCII charset
#define CHARSET_ADDRESS (0x9000U)
#define COLOUR_RAM_ADDRESS (0x1f800)
/* The same colour RAM by its 28-bit address, for lfill/lcopy. */
#define COLOUR_RAM_28BIT (0xff80000U)
// 16-bit text mode: 40 columns of two bytes, 25 rows.  No SCREEN_COLS here --
// freeze_sprited.c defines that as 80 for its own layout.
constexpr uint8_t SCREEN_ROW_BYTES = 80;
constexpr uint8_t SCREEN_ROWS = 25;
#define SCREEN_BYTES (SCREEN_ROW_BYTES * SCREEN_ROWS)

#define FOOTER_ADDRESS (SCREEN_ADDRESS + 24 * SCREEN_ROW_BYTES)

/* Only MONITOR links fdisk_screen_monitor.c, and it shows just these two.  The
 * sprite editor draws its own footer, so a message for it here was never
 * reachable -- and display_footer() indexes the table at runtime, so an unused
 * entry costs its full 80 columns that --gc-sections cannot reclaim. */
constexpr uint8_t FOOTER_COPYRIGHT = 0;
constexpr uint8_t FOOTER_FATAL = 1;
constexpr uint8_t FOOTER_MAX = 1;

constexpr uint8_t ATTRIB_REVERSE = 0x20;
constexpr uint8_t ATTRIB_BLINK = 0x10;
constexpr uint8_t ATTRIB_UNDERLINE = 0x80;
constexpr uint8_t ATTRIB_HIGHLIGHT = 0x40;

constexpr uint8_t COLOUR_BLACK = 0;
constexpr uint8_t COLOUR_WHITE = 1;
constexpr uint8_t COLOUR_RED = 2;
constexpr uint8_t COLOUR_CYAN = 3;
constexpr uint8_t COLOUR_PURPLE = 4;
constexpr uint8_t COLOUR_GREEN = 5;
constexpr uint8_t COLOUR_BLUE = 6;
constexpr uint8_t COLOUR_YELLOW = 7;
constexpr uint8_t COLOUR_ORANGE = 8;
constexpr uint8_t COLOUR_BROWN = 9;
constexpr uint8_t COLOUR_PINK = 10;
constexpr uint8_t COLOUR_GREY1 = 11;
constexpr uint8_t COLOUR_DARKGREY = 11;
constexpr uint8_t COLOUR_GREY2 = 12;
constexpr uint8_t COLOUR_GREY = 12;
constexpr uint8_t COLOUR_MEDIUMGREY = 12;
constexpr uint8_t COLOUR_LIGHTGREEN = 13;
constexpr uint8_t COLOUR_LIGHTBLUE = 14;
constexpr uint8_t COLOUR_GREY3 = 15;
constexpr uint8_t COLOUR_LIGHTGREY = 15;

void screen_hex(unsigned int addr, long value);
void screen_decimal(unsigned int addr, unsigned int value);

void format_hex(char* out, const long value, const char columns);

extern unsigned int screen_line_address;
extern char screen_column;

/* The VIC-IV setup every menu target shares: border, sprites off, screen and
 * charset base, VIC bank.  Text mode differs per target, so callers set ctrlc
 * and friends themselves afterwards. */
void setup_menu_screen_base(void);
void clear_colour_ram(void);
