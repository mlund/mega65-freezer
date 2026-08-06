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

/* Which footer row display_footer() shows. */
enum Footer : uint8_t {
    FooterCopyright = 0,
    FooterFatal = 1,
    FooterMax = 1,
};

/* Colour-RAM attribute bits, OR-ed into a colour. */
enum : uint8_t {
    AttribReverse = 0x20,
    AttribBlink = 0x10,
    AttribUnderline = 0x80,
    AttribHighlight = 0x40,
};

/* The sixteen palette entries.  Several have more than one common name. */
enum Colour : uint8_t {
    ColourBlack = 0,
    ColourWhite = 1,
    ColourRed = 2,
    ColourCyan = 3,
    ColourPurple = 4,
    ColourGreen = 5,
    ColourBlue = 6,
    ColourYellow = 7,
    ColourOrange = 8,
    ColourBrown = 9,
    ColourPink = 10,
    ColourGrey1 = 11,
    ColourDarkGrey = 11,
    ColourGrey2 = 12,
    ColourGrey = 12,
    ColourMediumGrey = 12,
    ColourLightGreen = 13,
    ColourLightBlue = 14,
    ColourGrey3 = 15,
    ColourLightGrey = 15,
};
