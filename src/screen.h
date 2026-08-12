#pragma once

#include "dma.h"
#include "format.h"

#include <stdint.h>

constexpr uint16_t SCREEN_ADDRESS = 0xB800;
/* Where the VIC finds its glyphs.  In VIC bank $8000 this is the character-ROM
 * shadow, not RAM, so the screen always renders the C64 ROM charset and text
 * written here must be screen codes.  Nothing can be loaded over it. */
constexpr uint16_t CHARSET_ADDRESS = 0x9000;
/* The window onto the first 2 KB of the 32 KB of colour memory at $FF80000.
 * It does not follow the VIC-IV's colour start ($D064/$D065), so it is the
 * displayed colour only while that offset is zero.  Wider than 16 bits, so it
 * carries the whole expression it appears in into 32-bit arithmetic. */
constexpr Addr28 COLOUR_RAM_ADDRESS = 0x1f800;
// 40 columns of two bytes in the 16-bit text mode, 80 of one in the 8-bit.
constexpr uint8_t SCREEN_ROW_BYTES = 80;
constexpr uint8_t SCREEN_ROWS = 25;
/* 40 where a cell is two bytes, 80 where it is one. */
constexpr uint8_t SCREEN_COLS = SCREEN_ROW_BYTES / SCREEN_CELL_BYTES;
constexpr uint16_t SCREEN_BYTES = SCREEN_ROW_BYTES * SCREEN_ROWS;

constexpr uint16_t FOOTER_ADDRESS = SCREEN_ADDRESS + 24 * SCREEN_ROW_BYTES;

/* Bytes per character cell: 2 in the 16-bit text mode, 1 in the 80-column mode.
 * Set per target in src/CMakeLists.txt, because this file is shared and
 * SCREEN_ROW_BYTES is 80 either way -- 40 cells of two bytes, or 80 of one.
 * It is also the DMA destination step for both planes, which is what lets one
 * renderer serve both geometries. */
#ifndef SCREEN_CELL_BYTES
#error "SCREEN_CELL_BYTES must be defined per target"
#endif

/* A menu is a stream of positioned fragments:
 *
 *     [len] [cell lo] [cell hi] [colour] [len screen codes ...]  ... [0]
 *
 * The run length is the text length, so the two cannot disagree, and a single
 * zero byte ends the stream.  The codes are already screen codes -- the
 * conversion happens at compile time -- so nothing here folds case.
 *
 * The position is stored as a byte offset from the top left rather than as a
 * column and a row, because it is known when the fragment is built: y * 80 + x
 * * 2 is a multiply the 6502 would otherwise open-code on every draw, and it
 * occupies the same two bytes either way.
 *
 * In the 16-bit mode the character is the low byte of its cell and the colour
 * is the high byte of the colour cell, hence the SCREEN_CELL_BYTES - 1 offset
 * into colour RAM; in the 8-bit mode both are simply the cell. */
#define SCREEN_CELL(x, y) ((uint16_t)((y) * SCREEN_ROW_BYTES + (x) * SCREEN_CELL_BYTES))

/* HORIZONTAL ONE EIGHTH BLOCK-4: the glyph every menu's horizontal rule uses. */
constexpr uint8_t MENU_RULE_GLYPH = 0x43;

/* A rule is `width` copies of one glyph, dimmed -- a divider separates
 * without saying anything itself. */
void draw_rule(uint16_t cell, uint8_t width);

/* Clears 16-bit text mode screen: manually clears the first couple of chars
 * (two, because of the DMA engine's own pipelining) then DMA-copies that to
 * fill the rest. */
void blank_screen(void);

/* Screen RAM, typed the way the SDK's own hardware registers are (VICIV,
 * CIA1, SID1...) rather than reached through POKE's bare address cast.  A
 * macro, not a fixed-address object like VICIV: the base is programmed at
 * runtime by setup_menu_screen_base(), though every caller here only ever
 * uses SCREEN_ADDRESS. */
#define SCREEN ((volatile uint8_t*)SCREEN_ADDRESS)

void draw_fragment(uint16_t cell, uint8_t colour, const uint8_t* codes, uint8_t length);
void draw_fragments(const uint8_t* stream);

/* The same, for text that is not known until run time -- a value read from the
 * machine, a name out of the process descriptor, a formatted number.  Converts
 * as it goes, where a fragment carries codes converted at compile time. */
void draw_text(uint16_t cell, uint8_t colour, const char* text, uint8_t length);

void screen_hex(uint16_t addr, long value);
void screen_decimal(uint16_t addr, uint16_t value);
void draw_decimal(uint16_t cell, uint8_t colour, uint16_t value);

extern uint16_t screen_line_address;
extern char screen_column;

/* The VIC-IV setup every menu target shares: border, sprites off, screen and
 * charset base, VIC bank.  Text mode differs per target, so callers set ctrlc
 * and friends themselves afterwards. */
void setup_menu_screen_base(void);
void clear_colour_ram(void);

/* One colour cell.  Colour RAM is above the 64 KB window, so unlike SCREEN it
 * cannot be a pointer; the cell index keeps the call cheap, see screen.c. */
void colour_poke(uint16_t cell, uint8_t value);

/* Which footer row display_footer() shows.  Kept to what MONITOR actually
 * displays: the table is indexed at run time, so an entry nothing selects still
 * costs its full 80 columns and --gc-sections cannot reclaim it. */
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

/* C++-only: the builders that turn menu text into the stream above.  Same
 * arrangement as helper.h, which serves C and the assembler from one file --
 * keeping the format's two halves together is the point, so a builder and the
 * walker in screen.c cannot drift apart.
 *
 * charset.h is stricter than the runtime conversion it replaces, usefully: a
 * plain "cccc" is rejected, because the block graphic the rules are drawn with
 * is not the letter c.  Spell it U"\U0001FB78". */
#ifdef __cplusplus

#include <charset.h>
#include <stddef.h>

namespace menu {

template <size_t N> struct Bytes {
    uint8_t data[N];
};

/* N counts the literal's terminating NUL, which a fragment does not store, so
 * the record is four header bytes plus N - 1 of text.  Taking the literal by
 * array reference is what deduces N, and is why the stored length cannot
 * disagree with the text.  Ch is deduced too, so "..." and U"..." -- the way a
 * block graphic is named -- both land here; charset.h converts either. */
template <typename Ch, size_t N>
consteval Bytes<N + 3> fragment(uint8_t x, uint8_t y, uint8_t colour, const Ch (&text)[N]) {
    const charset_impl::UnshiftedVideoString<N> codes{text};
    const uint16_t cell = SCREEN_CELL(x, y);
    Bytes<N + 3> f{{uint8_t(N - 1), uint8_t(cell & 0xff), uint8_t(cell >> 8), colour}};
    for (size_t i = 0; i < N - 1; ++i) {
        f.data[i + 4] = uint8_t(codes.Str[i]);
    }
    return f;
}

/* As fragment(), but works the column out from the text.  A banner carrying a
 * release number cannot have its column typed in beside it: the number changes
 * width between releases and the hand-computed column would silently stop
 * centring it. */
template <typename Ch, size_t N>
consteval Bytes<N + 3> centered(uint8_t y, uint8_t colour, const Ch (&text)[N]) {
    static_assert(N - 1 <= SCREEN_COLS, "text is wider than the screen");
    return fragment(uint8_t((SCREEN_COLS - (N - 1)) / 2), y, colour, text);
}

/* Concatenates the fragments and appends the zero length that ends the walk. */
template <size_t... Ns> consteval Bytes<(Ns + ... + 0) + 1> stream(Bytes<Ns>... parts) {
    Bytes<(Ns + ... + 0) + 1> out{};
    size_t at = 0;
    (
        [&] {
            for (size_t i = 0; i < Ns; ++i) {
                out.data[at++] = parts.data[i];
            }
        }(),
        ...);
    out.data[at] = 0;
    return out;
}

} // namespace menu

#endif // __cplusplus
