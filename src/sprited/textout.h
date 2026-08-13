#pragma once

#include "dma.h"

#include <stdbool.h>
#include <stdint.h>

/* SPRITED's text output, in place of mega65-libc's conio.
 *
 * A cursor and a current colour, which is the idiom the editor is written in;
 * screen.c takes the cell and the colour as arguments instead.  Underneath it
 * is screen.c all the same -- the 80x25 screen at $B800, the colour RAM, the
 * scheme colours.
 *
 * Only the charset differs: the editor patches its own glyphs into a copy, so
 * it cannot render from the character-ROM shadow the others use.
 */

/* Where that copy lives.  Reached by CHARPTR, which the legacy $D018 nibbles
 * could not: they cannot leave the VIC bank. */
constexpr Addr28 EDITOR_CHARSET = 0x15000;

typedef struct {
    uint8_t left, top, right, bottom;
} RECT;

void sprited_screen_init(void);

void bordercolor(uint8_t colour);
void bgcolor(uint8_t colour);
void fillrect(const RECT* rc, uint8_t code, uint8_t colour);

void gotoxy(uint8_t x, uint8_t y);
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
void cputdec(uint16_t value);
void cputhex(uint32_t value, uint8_t digits);

uint8_t cgetc(void);
bool kbhit(void);
void flushkeybuf(void);
