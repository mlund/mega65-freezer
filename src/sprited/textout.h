#pragma once

#include <stdbool.h>
#include <stdint.h>

/* SPRITED's text output, in place of mega65-libc's conio.
 *
 * The names and signatures are conio's because the editor is written against
 * that idiom -- a cursor and a current colour, rather than the cell-and-colour
 * argument the other tools' screen.c takes.  Keeping them is what let the
 * change be made without touching a hundred call sites, and the screen the
 * editor draws is pinned by test/verify_sprited_xemu.py either way.
 *
 * Underneath it is screen.c: the same 80x25 screen at $B800, the same colour
 * RAM, the same scheme colours.  Only the charset differs, because the editor
 * patches its own glyphs into a copy and so cannot use the ROM shadow the
 * others render from.
 */

typedef struct tagRECT {
    uint8_t left, top, right, bottom;
} RECT;

void sprited_screen_init(void);

void bordercolor(uint8_t colour);
void bgcolor(uint8_t colour);
void fillrect(const RECT* rc, uint8_t code, uint8_t colour);

void gotoxy(uint8_t x, uint8_t y);
void gotoy(uint8_t y);
void gohome(void);

void textcolor(uint8_t colour);
void revers(uint8_t on);
void blink(uint8_t on);
void clearattr(void);

void clrscr(void);
void cputc(uint8_t code);
void cputs(const char* text);
void cputcxy(uint8_t x, uint8_t y, uint8_t code);
void cputsxy(uint8_t x, uint8_t y, const char* text);
void cputncxy(uint8_t x, uint8_t y, uint8_t count, uint8_t code);
void cputdec(uint16_t value, uint8_t positions, uint8_t pad);
void cputhex(uint32_t value, uint8_t digits);

uint8_t cgetc(void);
bool kbhit(void);
void flushkeybuf(void);
