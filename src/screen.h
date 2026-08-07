#pragma once

#include "dma.h"

#include <stdint.h>

constexpr uint16_t SCREEN_ADDRESS = 0xB800;
// Use default char set, not ASCII charset
constexpr uint16_t CHARSET_ADDRESS = 0x9000;
/* The window onto the first 2 KB of the 32 KB of colour memory at $FF80000.
 * It does not follow the VIC-IV's colour start ($D064/$D065), so it is the
 * displayed colour only while that offset is zero.  Wider than 16 bits, so it
 * carries the whole expression it appears in into 32-bit arithmetic. */
constexpr Addr28 COLOUR_RAM_ADDRESS = 0x1f800;
// 16-bit text mode: 40 columns of two bytes, 25 rows.  No SCREEN_COLS here --
// sprited/editor.c defines that as 80 for its own layout.
constexpr uint8_t SCREEN_ROW_BYTES = 80;
constexpr uint8_t SCREEN_ROWS = 25;
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
 *     [len] [x] [y] [colour] [len screen codes ...]  ... [0]
 *
 * The run length is the text length, so the two cannot disagree, and a single
 * zero byte ends the stream.  The codes are already screen codes -- the
 * conversion happens at compile time -- so nothing here folds case.
 *
 * In the 16-bit mode the character is the low byte of its cell and the colour
 * is the high byte of the colour cell, hence the SCREEN_CELL_BYTES - 1 offset
 * into colour RAM; in the 8-bit mode both are simply the cell. */
void draw_fragment(uint8_t x, uint8_t y, uint8_t colour, const uint8_t* codes, uint8_t length);
void draw_fragments(const uint8_t* stream);

/* Only MONITOR links monitor/console.c, and it shows just these two.  The
 * sprite editor draws its own footer, so a message for it here was never
 * reachable -- and display_footer() indexes the table at runtime, so an unused
 * entry costs its full 80 columns that --gc-sections cannot reclaim. */

void screen_hex(uint16_t addr, long value);
void screen_decimal(uint16_t addr, uint16_t value);

void format_hex(char* out, const long value, const char columns);

extern uint16_t screen_line_address;
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
    Bytes<N + 3> f{{uint8_t(N - 1), x, y, colour}};
    for (size_t i = 0; i < N - 1; ++i) {
        f.data[i + 4] = uint8_t(codes.Str[i]);
    }
    return f;
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
