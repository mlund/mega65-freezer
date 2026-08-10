#include "textout.h"

#include "colours.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"

#include <mega65.h>

/* 80 columns of one byte, which is what SCREEN_CELL_BYTES = 1 makes of
 * screen.h's 80-byte row. */
constexpr uint8_t TEXT_COLUMNS = SCREEN_ROW_BYTES;

/* The editor patches its own glyphs in, so it renders from a copy in RAM
 * rather than the character-ROM shadow the other tools use.  CHARPTR reaches
 * it; the legacy $D018 nibbles could not, being confined to the VIC bank. */
constexpr Addr28 EDITOR_CHARSET = 0x15000;

static uint8_t cursor_x;
static uint8_t cursor_y;
static uint8_t colour = SchemeText;
static uint8_t attributes;

static uint16_t cell_at(uint8_t x, uint8_t y) {
    return (uint16_t)((uint16_t)y * SCREEN_ROW_BYTES + x);
}

/* Screen codes, not ASCII: the charset is copied from CHARSET A, where the
 * letters sit where a screen code says rather than where ASCII does. */
static uint8_t screen_code(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (uint8_t)(c - 0x40);
    }
    if (c >= 'a' && c <= 'z') {
        return (uint8_t)(c - 0x60);
    }
    return (uint8_t)c;
}

static void put_cell(uint16_t cell, uint8_t code) {
    SCREEN[cell] = code;
    colour_poke(cell, colour | attributes);
}

void sprited_screen_init(void) {
    m65_io_enable();

    /* $D054 before the hot registers: their recalculation takes the row width
     * from its CHR16 bit, and this screen is 8-bit. */
    VICIV.ctrlc = VICIV.ctrlc & VIC4_CTRLC_MODE_MASK;
    VICIV.ctrlb = VIC4_CTRLB_80_COLUMN;
    /* $C9, not $C8: bit 0 is the H640 X-scroll correction 80 columns need. */
    VICIV.ctrl2 = 0xC9;
    VICIV.sdbdrwd_lsb = VIC4_SIDE_BORDER_WIDTH;
    VICIV.sdbdrwd_msb = VIC4_BORDER_MSB_HOTREG;
    VICIV.spr_ena = 0x00;

    VICIV.scrnptr = SCREEN_ADDRESS;
    VICIV.charptr = EDITOR_CHARSET;

    clearattr();
    clrscr();
}

void bordercolor(uint8_t new_colour) {
    VICIV.bordercol = new_colour;
}

void bgcolor(uint8_t new_colour) {
    VICIV.screencol = new_colour;
}

/* Inclusive of `bottom`, exclusive of `right`, as conio's was: the callers are
 * written to that and the screen they draw is pinned. */
void fillrect(const RECT* rc, uint8_t code, uint8_t fill_colour) {
    const uint8_t width = (uint8_t)(rc->right - rc->left);

    for (uint8_t row = rc->top; row <= rc->bottom; row++) {
        const uint16_t cell = cell_at(rc->left, row);
        lfill_skip(SCREEN_ADDRESS + cell, code, width, SCREEN_CELL_BYTES);
        lfill_skip(COLOUR_RAM_ADDRESS + cell, fill_colour, width, SCREEN_CELL_BYTES);
    }
}

void gotoxy(uint8_t x, uint8_t y) {
    cursor_x = x;
    cursor_y = y;
}

void gotoy(uint8_t y) {
    cursor_y = y;
}

void gohome(void) {
    cursor_x = 0;
    cursor_y = 0;
}

void textcolor(uint8_t new_colour) {
    colour = new_colour;
}

void revers(uint8_t on) {
    attributes = on ? (attributes | AttribReverse) : (attributes & (uint8_t)~AttribReverse);
}

void blink(uint8_t on) {
    attributes = on ? (attributes | AttribBlink) : (attributes & (uint8_t)~AttribBlink);
}

void clearattr(void) {
    attributes = 0;
}

void clrscr(void) {
    blank_screen();
    clear_colour_ram();
    gohome();
}

void cputc(uint8_t code) {
    put_cell(cell_at(cursor_x, cursor_y), code);
    if (++cursor_x >= TEXT_COLUMNS) {
        cursor_x = 0;
        cursor_y++;
    }
}

void cputs(const char* text) {
    while (*text) {
        cputc(screen_code(*text++));
    }
}

void cputcxy(uint8_t x, uint8_t y, uint8_t code) {
    gotoxy(x, y);
    cputc(code);
}

void cputsxy(uint8_t x, uint8_t y, const char* text) {
    gotoxy(x, y);
    cputs(text);
}

/* One strided fill per plane rather than a call per cell: a run is one glyph
 * in one colour, which is what lfill_skip is for. */
void cputncxy(uint8_t x, uint8_t y, uint8_t count, uint8_t code) {
    const uint16_t cell = cell_at(x, y);

    lfill_skip(SCREEN_ADDRESS + cell, code, count, SCREEN_CELL_BYTES);
    lfill_skip(COLOUR_RAM_ADDRESS + cell, colour | attributes, count, SCREEN_CELL_BYTES);
    gotoxy((uint8_t)(x + count), y);
}

/* Right-aligned in `positions` when one is given, which is what the readouts
 * want; `pad` is the glyph to fill with, 0 meaning space. */
void cputdec(uint16_t value, uint8_t positions, uint8_t pad) {
    uint8_t digits[5];
    uint8_t count = 0;

    do {
        digits[count++] = (uint8_t)('0' + value % 10);
        value /= 10;
    } while (value);

    while (positions > count) {
        cputc(pad ? pad : ' ');
        positions--;
    }
    while (count) {
        cputc(screen_code((char)digits[--count]));
    }
}

/* Prefixed with `$`, as conio's was: the callers pass a digit count and expect
 * the marker, so the readouts read `$000C000`. */
void cputhex(uint32_t value, uint8_t digits) {
    cputc('$');
    while (digits--) {
        const uint8_t nybble = (uint8_t)((value >> (digits * 4)) & 0x0F);
        cputc(screen_code((char)(nybble < 10 ? '0' + nybble : 'A' + nybble - 10)));
    }
}

uint8_t cgetc(void) {
    uint8_t key;

    while (!(key = ASCIIKEY)) {
    }
    ASCIIKEY = 0;
    return key;
}

bool kbhit(void) {
    return ASCIIKEY != 0;
}

void flushkeybuf(void) {
    ASCIIKEY = 0;
}
