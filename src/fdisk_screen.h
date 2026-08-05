#ifndef __FDISK_SCREEN_H__
#define __FDISK_SCREEN_H__

#define SCREEN_ADDRESS (0xB800U)
// Use default char set, not ASCII charset
#define CHARSET_ADDRESS (0x9000U)
#define COLOUR_RAM_ADDRESS (0x1f800)
// 16-bit text mode: 40 columns of two bytes, 25 rows.
#define SCREEN_COLS 40
#define SCREEN_ROW_BYTES 80
#define SCREEN_ROWS 25
#define SCREEN_BYTES (SCREEN_ROW_BYTES * SCREEN_ROWS)

#define FOOTER_ADDRESS (SCREEN_ADDRESS + 24 * SCREEN_ROW_BYTES)

#define FOOTER_COPYRIGHT 0
#define FOOTER_SPRITED 1
#define FOOTER_BLANK 2
#define FOOTER_FATAL 3
#define FOOTER_MAX 3

#define ATTRIB_REVERSE 0x20
#define ATTRIB_BLINK 0x10
#define ATTRIB_UNDERLINE 0x80
#define ATTRIB_HIGHLIGHT 0x40

#define COLOUR_BLACK 0
#define COLOUR_WHITE 1
#define COLOUR_RED 2
#define COLOUR_CYAN 3
#define COLOUR_PURPLE 4
#define COLOUR_GREEN 5
#define COLOUR_BLUE 6
#define COLOUR_YELLOW 7
#define COLOUR_ORANGE 8
#define COLOUR_BROWN 9
#define COLOUR_PINK 10
#define COLOUR_GREY1 11
#define COLOUR_DARKGREY 11
#define COLOUR_GREY2 12
#define COLOUR_GREY 12
#define COLOUR_MEDIUMGREY 12
#define COLOUR_LIGHTGREEN 13
#define COLOUR_LIGHTBLUE 14
#define COLOUR_GREY3 15
#define COLOUR_LIGHTGREY 15

void screen_hex(unsigned int addr, long value);
void screen_decimal(unsigned int addr, unsigned int value);

void format_hex(char* out, const long value, const char columns);

extern long screen_line_address;
extern char screen_column;

#endif /* __FDISK_SCREEN_H__ */
