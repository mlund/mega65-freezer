/*
 * SPRED65 - The MEGA65 sprite editor
 *
 * Copyright (c) 2020-2021 Hernán Di Pietro, Paul Gardner-Stephen.
 *
 *  This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

    Version   0.10
    Date      2021-05-28

    CHANGELOG

    v0.5        Uses conio for proper initialization and some of its
                new features.  Color selection with MEGA/CTRL keys.

    v0.6        FIX: Screen moved to $12000.
                Multicolor and 16-color sprite support.
                New color selection UI.
                Change sprite type on-the-fly.with * key.
                Clear sprite key.

    v0.8        80x50 screen mode option, new UI, redraw optimizations.
                Redesigned key scheme.
                Supports 16-bit sprite data pointers (SPRPTR16).
                Honours VIC-II Bank bits ($DD00) if SPRPTR16 is OFF.
                Honours VIC color registers.
                Supports extended width sprites (SPRX64EN)
                16-color sprite uses 64-bit implicit width.
                Drawing tools: pixel, box, circle ,lines.
                Sprite Test Mode.

    v0.9        Transfer to/from frozen sprite memory of registers and data.
                Display: 50-line mode, wide-screen/4:3 aspect ratio modes.
                UI Enhancements and fixes.
                Fixes buffer overrun in Ask() function.
                Sprite preview at side.
                Fancy editing cursor sprite.
                Fancy pointer sprite
                H/V expand toggle
                Size optimizations
                Redrawing optimizations
                Fetch/Store slot shortcuts.

    v0.10       90-column display mode by default.
                Faster I/O to Freeze memory (fetch_sector_32 functions)
                Fixed Copy Sprite bug
                Fixed 64bit-width sprite preview.
                Changes at HELP screen
                Cursor updates optimization and fix w/SPRX64EN activated
                Fix "K" key for 16color



    TODO:
    * Consider SPRBPMEN for 16-color sprites
 */
#include "cc65compat.h"
#include "freezer.h"

#include <mega65.h>
#include <mega65/conio.h>
#include <mega65/hal.h>
#include <mega65/memory.h>
#include <mega65/mouse.h>
#include <stdio.h>
#include <stdlib.h>

// #define SPRITED_STANDALONE

/* $D000 as the running editor sees it.  Its own display -- the preview sprite,
 * the edit cursor -- is poked here, live, whatever is being edited. */
constexpr uint16_t LOCAL_VIC_BASE = 0xD000;

/* The sprite registers being edited, and the pair that reaches them.  Built
 * into the freezer they are the frozen program's registers, which live in the
 * freeze slot on the SD card rather than in memory: reg_peek() goes out
 * through freeze_peek()'s sector I/O, which is why VIC_BASE cannot be the
 * address of an MMIO struct however much it looks like one. */
#ifdef SPRITED_STANDALONE
constexpr uint32_t VIC_BASE = LOCAL_VIC_BASE;
constexpr uint32_t CIA2_PORT_A = 0xDD00UL;

static inline uint8_t reg_peek(uint32_t address) {
    return PEEK(address);
}
static inline void reg_poke(uint32_t address, uint8_t value) {
    POKE(address, value);
}
#else
constexpr uint32_t VIC_BASE = 0xFFD3000UL; // Where VIC-II is mapped in frozen memory
constexpr uint32_t CIA2_PORT_A = 0xFFD3D00UL;

static inline uint8_t reg_peek(uint32_t address) {
    return freeze_peek(address);
}
static inline void reg_poke(uint32_t address, uint8_t value) {
    freeze_poke(address, value);
}
#endif

constexpr uint32_t REG_SPRPTR = VIC_BASE + 0x6C; /* SPRPTR0-2 occupy $6C-$6E */
constexpr uint32_t REG_SPR_VEXPAND = VIC_BASE + 0x17;
constexpr uint32_t REG_SPR_HEXPAND = VIC_BASE + 0x1D;
constexpr uint32_t REG_SPR_16COL = VIC_BASE + 0x6B;
constexpr uint32_t REG_SPR_MULTICOLOR = VIC_BASE + 0x1C;
constexpr uint32_t REG_SPRX64EN = VIC_BASE + 0x57;
constexpr uint32_t REG_SPRITE_MULTICOL1 = VIC_BASE + 0x25;
constexpr uint32_t REG_SPRITE_MULTICOL2 = VIC_BASE + 0x26;
constexpr uint32_t REG_SPRPALSEL = VIC_BASE + 0x70;

static inline uint32_t reg_sprite_color(uint8_t sprite) {
    return VIC_BASE + 0x27 + sprite;
}

constexpr uint16_t LOCAL_REG_SPR_16COL = LOCAL_VIC_BASE + 0x6B;
constexpr uint16_t LOCAL_REG_SPR_MULTICOLOR = LOCAL_VIC_BASE + 0x1C;
constexpr uint16_t LOCAL_REG_SPRITE_MULTICOL1 = LOCAL_VIC_BASE + 0x25;
constexpr uint16_t LOCAL_REG_SPRITE_MULTICOL2 = LOCAL_VIC_BASE + 0x26;
/* Only the disabled X-width path uses this one; see issue #77. */
[[maybe_unused]] constexpr uint16_t LOCAL_REG_SPRX64EN = LOCAL_VIC_BASE + 0x57;

static inline uint16_t local_reg_sprite_color(uint8_t sprite) {
    return LOCAL_VIC_BASE + 0x27 + sprite;
}

[[maybe_unused]] static inline uint8_t sprite_palette(void) {
    return (uint8_t)((reg_peek(REG_SPRPALSEL) & 0x30) >> 4);
}

static inline bool is_sprite_multicolor(uint8_t sprite) {
    return reg_peek(REG_SPR_MULTICOLOR) & (1 << sprite);
}
static inline bool is_sprite_16color(uint8_t sprite) {
    return reg_peek(REG_SPR_16COL) & (1 << sprite);
}
static inline bool is_sprite_xwidth(uint8_t sprite) {
    return reg_peek(REG_SPRX64EN) & (1 << sprite);
}
static inline bool is_sprite_hexpand(uint8_t sprite) {
    return reg_peek(REG_SPR_HEXPAND) & (1 << sprite);
}
static inline bool is_sprite_vexpand(uint8_t sprite) {
    return reg_peek(REG_SPR_VEXPAND) & (1 << sprite);
}

/* Where the frozen program keeps its sprite pointer table.  Bit 7 of SPRPTR2
 * is the SPRPTR16 flag, not an address bit. */
static inline uint32_t sprite_pointer_addr(void) {
    return (uint32_t)reg_peek(REG_SPRPTR) | ((uint32_t)reg_peek(REG_SPRPTR + 1) << 8) |
        ((uint32_t)(reg_peek(REG_SPRPTR + 2) & 0x7F) << 16);
}

/* SPRPTR16: pointers are 16-bit words indexing 64-byte blocks, rather than
 * bytes indexing within the VIC-II bank CIA2 selects. */
static inline bool sprite_pointers_are_16bit(void) {
    return reg_peek(REG_SPRPTR + 2) & 0x80;
}

static inline uint16_t sprite_size_bytes(uint8_t sprite) {
    return (is_sprite_xwidth(sprite) || is_sprite_16color(sprite)) ? 168 : 64;
}

static inline uint32_t sprite_data_addr(uint8_t sprite) {
    if (sprite_pointers_are_16bit()) {
        return 64 *
            (((uint32_t)reg_peek(sprite_pointer_addr() + 1 + sprite * 2) << 8) +
                (uint32_t)reg_peek(sprite_pointer_addr() + sprite * 2));
    }
    return (uint32_t)(64 * reg_peek(sprite_pointer_addr() + sprite)) |
        ((uint32_t)(~reg_peek(CIA2_PORT_A) & 0x3) << 14);
}
// #define REG_SPRBPMEN_0_3            (vic_registers[0x49] >> 4)
// #define REG_SPRBPMEN_4_7            (vic_registers[0x4B] >> 4)
// #define SPRITE_BITPLANE_ENABLE(n)	(((REG_SPRBPMEN_4_7) << 4 | REG_SPRBPMEN_0_3) & (1 << (n)))
constexpr uint8_t SCREEN_ROWS = 25;
constexpr uint8_t SCREEN_COLS = 80;

constexpr uint8_t SPRITE_MAX_COUNT = 8;
constexpr uint8_t DEFAULT_BORDER_COLOR = 6;
constexpr uint8_t DEFAULT_SCREEN_COLOR = 6;
constexpr uint8_t DEFAULT_BACK_COLOR = 11;

constexpr uint8_t TRANS_CHARACTER = 230;
constexpr uint8_t SOLID_BLOCK_CHARACTER = 224;
constexpr uint8_t SHAPE_PREVIEW_CHARACTER = 32;
constexpr uint8_t SIDEBAR_COLUMN = 65;
constexpr uint8_t SIDEBAR_WIDTH = SCREEN_COLS - SIDEBAR_COLUMN;
constexpr uint8_t SIDEBAR_PREVIEW_AREA_TOP = 10;
constexpr uint8_t SIDEBAR_PREVIEW_AREA_BOTTOM = 20;
constexpr uint8_t SIDEBAR_PREVIEW_AREA_HEIGHT =
    SIDEBAR_PREVIEW_AREA_BOTTOM - SIDEBAR_PREVIEW_AREA_TOP;
constexpr uint8_t SPRITE_OFFSET_X = 24;
constexpr uint8_t SPRITE_OFFSET_Y = 50;

constexpr uint16_t JOY_DELAY = 10000U;

/* These stay macros: they are type-generic, and C has no other spelling of
 * that without _Generic. */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

// Screen RAM for our area. Werror do not use 16-bit character mode
// so we need 80x25 = 2K area.
constexpr uint32_t SCREEN_RAM_ADDRESS = 0x12000UL;
constexpr uint32_t CHARSET_ADDRESS = 0x15000UL;
constexpr uint32_t SPRITE_POINTER_TABLE = 0x16000UL;
constexpr uint32_t SPRITE_BUFFER = 0x40000UL;
constexpr uint8_t PREVIEW_SPRITE_NUM = 2;
constexpr uint8_t EDIT_CURSOR_NUM = 1;

/* What still needs drawing, OR-ed together into redraw_flags.  Anonymous
 * because the bits combine, so they never name a type of their own. */
enum : uint8_t {
    RedrawNothing = 0,
    RedrawSidebarInfo = 1,
    RedrawSidebarCoords = 2,
    RedrawSidebarColor = 4,
    RedrawSidebarTools = 8,
    RedrawToolPreview = 16,
    RedrawHeader = 32,
    RedrawAll = 0x3f,
};

/* mega65-libc types screen text as uint8_t*, while C string literals are
 * char[].  These convert once so call sites stay free of casts. */
static inline void screen_puts(const char* s) {
    cputs((const uint8_t*)s);
}
static inline void screen_putsxy(uint8_t x, uint8_t y, const char* s) {
    cputsxy(x, y, (const uint8_t*)s);
}

/* How the sprite being edited stores its pixels. */
enum SpriteColorMode : uint8_t {
    SpriteColorModeMono = 0,
    SpriteColorModeMulti = 1,
    SpriteColorMode16 = 2,
};

/* The drawing tools, in the order CHSET_TOOLBOX draws their buttons. */
enum DrawingTool : uint8_t {
    DrawingToolPixel = 0,
    DrawingToolLine = 1,
    DrawingToolBox = 2,
    DrawingToolFilledBox = 3,
    DrawingToolCircle = 4,       /* not implemented */
    DrawingToolFilledCircle = 5, /* not implemented */
};

/* Index into APPSTATE::color[]. */
enum ColorIndex : uint8_t {
    ColorBack = 0,
    ColorFore = 1,
    ColorMc1 = 2,
    ColorMc2 = 3,
};

typedef void (*PaintFunc)(uint8_t, uint8_t);

typedef struct TagAppstate {
    uint8_t wide_screen_mode;
    uint8_t sprite_number;
    enum SpriteColorMode sprite_color_mode;
    uint8_t sprite_width, sprite_height;
    uint8_t cells_per_pixel, bytes_per_row, pixels_per_byte;
    uint8_t color[4];
    uint8_t color_source[4];
    enum ColorIndex current_color_idx;
    uint8_t cursor_x, cursor_y;
    uint8_t canvas_left_x;
    enum DrawingTool drawing_tool;
    uint8_t tool_active, tool_org_x, tool_org_y, fill_shape;
    uint8_t redraw_flags; // See the Redraw... constants for the bits
    RECT redraw_rect;
    void (*draw_cell_fn)(uint8_t, uint8_t);
    void (*paint_cell_fn)(uint8_t, uint8_t);
    void (*draw_shape_fn)(PaintFunc);
    void (*update_cursor_x_fn)(void);
    void (*update_cursor_y_fn)(void);
    uint16_t sprite_size_bytes;
    /* A 28-bit address in the freeze slot, not in memory -- see
     * sprite_data_addr(). */
    uint32_t sprite_data_addr;
} APPSTATE;

typedef struct TagFileoptions {
    uint8_t mode;
    uint8_t name[16];
} FILEOPTIONS;

static APPSTATE g_state;

/* Nonstatic Function prototypes */
void update_sprite_parameters(bool);
void set_draw_tool(enum DrawingTool);
void set_redraw_full_canvas(void);
void set_effective_tool_rect(RECT*);
void setup_text_palette(void);
void update_cursor_x(void);
void update_cursor_y(void);
void update_cursor_xmsb(void);

/* Toolbox Character set, in order of the DrawingTool enumeration */

// clang-format off
// clang-format off
// clang-format off
static const uint8_t CHSET_TOOLBOX[] = {
    // ------ UPPER ROW -----
    // Pixel tool
    0b00000000,
    0b00000000,
    0b11111111,
    0b10000000,
    0b10000000,
    0b10000000,
    0b10000000,
    0b10000001,

    0b00000000,
    0b00000000,
    0b11111111,
    0b00000001,
    0b00000001,
    0b00000001,
    0b00000001,
    0b10000001,

    // Line tool
    0b00000000,
    0b00000000,
    0b11111111,
    0b10000000,
    0b10000000,
    0b10110000,
    0b10001100,
    0b10000011,

    0b00000000,
    0b00000000,
    0b11111111,
    0b00000001,
    0b00000001,
    0b00000001,
    0b00000001,
    0b00000001,

    // Box tool
    0b00000000,
    0b00000000,
    0b11111111,
    0b10000000,
    0b10000000,
    0b10011111,
    0b10010000,
    0b10010000,

    0b00000000,
    0b00000000,
    0b11111111,
    0b00000001,
    0b00000001,
    0b11111001,
    0b00001001,
    0b00001001,

    // Filled-box tool
    0b00000000,
    0b00000000,
    0b11111111,
    0b10000000,
    0b10000000,
    0b10001111,
    0b10001111,
    0b10001111,

    0b00000000,
    0b00000000,
    0b11111111,
    0b00000001,
    0b00000001,
    0b11110001,
    0b11110001,
    0b11110001,

    // Circle tool
    0b00000000,
    0b00000000,
    0b11111111,
    0b10000000,
    0b10000000,
    0b10000011,
    0b10000100,
    0b10001000,

    0b00000000,
    0b00000000,
    0b11111111,
    0b00000001,
    0b00000001,
    0b11000001,
    0b00100001,
    0b00010001,

    // Filled-circle tool
    0b00000000,
    0b00000000,
    0b11111111,
    0b10000000,
    0b10000000,
    0b10000001,
    0b10000111,
    0b10001111,

    0b00000000,
    0b00000000,
    0b11111111,
    0b00000001,
    0b00000001,
    0b10000001,
    0b11100001,
    0b11110001,

    // ------ LOWER ROW -------
    // Pixel tool
    0b10000001,
    0b10000000,
    0b10000000,
    0b10000000,
    0b10000000,
    0b11111111,
    0b00111111,
    0b00000000,

    0b10000001,
    0b00000001,
    0b00000001,
    0b00000001,
    0b00000001,
    0b11111111,
    0b11111111,
    0b00000000,

    // Line tool
    0b10000000,
    0b10000000,
    0b10000000,
    0b10000000,
    0b10000000,
    0b11111111,
    0b11111111,
    0b00000000,

    0b11000001,
    0b00110001,
    0b00001101,
    0b00000001,
    0b00000001,
    0b11111111,
    0b11111111,
    0b00000000,

    // Box tool
    0b10010000,
    0b10010000,
    0b10011111,
    0b10000000,
    0b10000000,
    0b11111111,
    0b11111111,
    0b00000000,

    0b00001001,
    0b00001001,
    0b11111001,
    0b00000001,
    0b00000001,
    0b11111111,
    0b11111111,
    0b00000000,

    // Filled-box tool
    0b10001111,
    0b10001111,
    0b10001111,
    0b10000000,
    0b10000000,
    0b11111111,
    0b11111111,
    0b00000000,

    0b11110001,
    0b11110001,
    0b11110001,
    0b00000001,
    0b00000001,
    0b11111111,
    0b11111111,
    0b00000000,

    // Circle tool
    0b10001000,
    0b10000100,
    0b10000011,
    0b10000000,
    0b10000000,
    0b11111111,
    0b11111111,
    0b00000000,

    0b00010001,
    0b00100001,
    0b11000001,
    0b00000001,
    0b00000001,
    0b11111111,
    0b11111111,
    0b00000000,

    // Filled-circle tool
    0b10001111,
    0b10000111,
    0b10000001,
    0b10000000,
    0b10000000,
    0b11111111,
    0b11111111,
    0b00000000,

    0b11110001,
    0b11100001,
    0b10000001,
    0b00000001,
    0b00000001,
    0b11111111,
    0b11111111,
    0b00000000
};
// clang-format on
// clang-format on
// clang-format on

#define TOOLBOX_CHARSET_BASE_IDX 232

// clang-format off
// clang-format off
// clang-format off
static const uint8_t SPRITE_POINTER[] = {

    0b10000000, 0b00000000, 0b00000000,
    0b11000000, 0b00000000, 0b00000000,
    0b11100000, 0b00000000, 0b00000000,
    0b11110000, 0b00000000, 0b00000000,
    0b11111000, 0b00000000, 0b00000000,
    0b11111100, 0b00000000, 0b00000000,
    0b11111110, 0b00000000, 0b00000000,
    0b11111111, 0b00000000, 0b00000000,
    0b11111000, 0b00000000, 0b00000000,
    0b11011000, 0b00000000, 0b00000000,
    0b10001100, 0b00000000, 0b00000000,
    0b00001100, 0b00000000, 0b00000000,
    0b00000110, 0b00000000, 0b00000000,
    0b00000110, 0b00000000, 0b00000000,
    0b00000011, 0b00000000, 0b00000000,
    0b00000011, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000
};
// clang-format on
// clang-format on
// clang-format on

// clang-format off
// clang-format off
// clang-format off
static const uint8_t EDIT_CURSORS[] = {

    // Cursor for single cell

    0b11110000, 0b00000000, 0b00000000,
    0b10010000, 0b00000000, 0b00000000,
    0b10010000, 0b00000000, 0b00000000,
    0b10010000, 0b00000000, 0b00000000,
    0b10010000, 0b00000000, 0b00000000,
    0b10010000, 0b00000000, 0b00000000,
    0b10010000, 0b00000000, 0b00000000,
    0b11110000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,

    // double cell

    0b11111111, 0b00000000, 0b00000000,
    0b10000001, 0b00000000, 0b00000000,
    0b10000001, 0b00000000, 0b00000000,
    0b10000001, 0b00000000, 0b00000000,
    0b10000001, 0b00000000, 0b00000000,
    0b10000001, 0b00000000, 0b00000000,
    0b10000001, 0b00000000, 0b00000000,
    0b11111111, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,

    // triple cell

    0b11111111, 0b11110000, 0b00000000,
    0b10000000, 0b00010000, 0b00000000,
    0b10000000, 0b00010000, 0b00000000,
    0b10000000, 0b00010000, 0b00000000,
    0b10000000, 0b00010000, 0b00000000,
    0b10000000, 0b00010000, 0b00000000,
    0b10000000, 0b00010000, 0b00000000,
    0b11111111, 0b11110000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,

    // four cell
    0b11111111, 0b11111111, 0b00000000,
    0b10000000, 0b00000001, 0b00000000,
    0b10000000, 0b00000001, 0b00000000,
    0b10000000, 0b00000001, 0b00000000,
    0b10000000, 0b00000001, 0b00000000,
    0b10000000, 0b00000001, 0b00000000,
    0b10000000, 0b00000001, 0b00000000,
    0b11111111, 0b11111111, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000,
    0b00000000, 0b00000000, 0b00000000
};
// clang-format on
// clang-format on
// clang-format on

static const uint8_t EDIT_CURSOR_COLOR_MAP[16] = {COLOUR_BLACK,
    COLOUR_BLUE,
    COLOUR_BROWN,
    COLOUR_GREY1,
    COLOUR_GREY2,
    COLOUR_GREY3,
    COLOUR_LIGHTGREEN,
    COLOUR_WHITE,
    COLOUR_WHITE,
    COLOUR_LIGHTGREEN,
    COLOUR_GREY3,
    COLOUR_GREY2,
    COLOUR_GREY1,
    COLOUR_RED,
    COLOUR_BROWN,
    COLOUR_BLUE};

static void set_rect(RECT* rc, uint8_t left, uint8_t top, uint8_t right, uint8_t bottom) {
    rc->left = left;
    rc->right = right;
    rc->top = top;
    rc->bottom = bottom;
}

static void initialize() {
    // Set 40MHz, VIC-IV I/O, 80 column, screen RAM @ $8000
    POKE(0, 65);

    // --- Freezer slot setup

    freeze_slot_start_sector = read_freeze_slot_start_sector(0);

    request_freeze_region_list();

    // --- Screen setup ----

    conioinit();

    sethotregs(1);

    setextendedattrib(1);
    setscreensize(SCREEN_COLS, SCREEN_ROWS);
    setscreenaddr(SCREEN_RAM_ADDRESS);
    bordercolor(DEFAULT_BORDER_COLOR);
    bgcolor(DEFAULT_SCREEN_COLOR);

    // --- Charset setup ----

    lcopy(0x2D800, CHARSET_ADDRESS, 2048);
    lcopy(
        (long)CHSET_TOOLBOX, CHARSET_ADDRESS + TOOLBOX_CHARSET_BASE_IDX * 8, sizeof(CHSET_TOOLBOX));
    setcharsetaddr(CHARSET_ADDRESS);

    // --- Sprite setup ----

    // Set pointer table to SPRITE_POINTER_TABLE
    // Set sprite 0 to our cursor. Address = $380.
    // Set sprite 1 to editing cursor #1   Address = $3C0
    // Set sprite 2 to current sprite placeholder.   Address = SPRITE_BUFFER

    // Set local sprite pointer table

    POKE(0xD06C, (uint8_t)SPRITE_POINTER_TABLE);
    POKE(0xD06D, (uint8_t)(SPRITE_POINTER_TABLE >> 8));
    POKE(0xD06E, (uint8_t)(SPRITE_POINTER_TABLE >> 16) | 128); // Enable SPRPTR16

    // #0: Mouse Pointer sprite at 0x380

    lcopy((long)SPRITE_POINTER, 0x380, 63);
    lpoke(SPRITE_POINTER_TABLE, 0x0E);
    lpoke(SPRITE_POINTER_TABLE + 1, 0x00);

    // Address of edit cursor
    lpoke(SPRITE_POINTER_TABLE + 2, 0x0F); // 64 * 0xF = 0x3C0
    lpoke(SPRITE_POINTER_TABLE + 3, 0x00);

    // Address of current sprite image preview
    lpoke(SPRITE_POINTER_TABLE + 4, SPRITE_BUFFER / 64 % 256);
    lpoke(SPRITE_POINTER_TABLE + 5, SPRITE_BUFFER / 64 / 256);

    // Sprite properties (color, initial pos, etc.)

    POKE(0xD074, 0); // Alpha OFF
    POKE(0xD076, 0); // V400 mode off for editor sprites.
    POKE(0xD077, 0); // Y-MSBs off
    POKE(0xD078, 0); // Y-MSBs off

    VICIV.spr_ena = 7; // Enable #0, #1, #2
    POKE(0xD01D, 0);   // H-expand off for editor sprites.
    POKE(0xD017, 0);   // V-expand off for editor sprites.
    POKE(0xD000, 100);
    POKE(0xD001, 100);

    POKE(0xD002, 0);
    POKE(0xD003, 0);

    POKE(0xD027, 7);
    POKE(0xD028, 1);
    POKE(0xD010, 1 << PREVIEW_SPRITE_NUM); // 8th bit for Sprite#2
    POKE(0xD01C, 0);                       // All mono/hires sprites
    POKE(0xD06B, 0);                       // 16-color mode OFF

    g_state.redraw_flags = RedrawAll;
    g_state.sprite_number = 0;
    g_state.cursor_x = g_state.cursor_y = 0;
    g_state.current_color_idx = ColorFore;
    g_state.tool_active = 0;
    g_state.tool_org_x = g_state.tool_org_y = 0;
    g_state.color[ColorBack] = DEFAULT_BACK_COLOR;
    g_state.wide_screen_mode = 0;
    g_state.update_cursor_x_fn = update_cursor_x;
    g_state.update_cursor_y_fn = update_cursor_y;

    set_draw_tool(DrawingToolPixel);
    update_sprite_parameters(true);
    set_redraw_full_canvas();

    g_state.update_cursor_x_fn();
    g_state.update_cursor_y_fn();
}

void load_slot_sprite_palette() {
}

void setup_text_palette(void) {
    // To properly display color in the editor, we set the main palette bank to 0,
    // the sprite palette bank, and use the alt palette to display text and UI.
    // Werror do this because ALTPAL is selected by VIC-III HIGHLIGHT+UNDERLINE
    // combination and  Foreground colors in alt-palette are used from the 16th index,
    // so we avoid fiddling with this.

    // POKE(0xD070UL, (PEEK(0xD070UL) & ~48) | ((reg_peek(REG_SPRPALSEL) & 0xC) << 2));
    // setmapedpal(sprite_palette());
}

void update_cursor_x() {
    uint8_t canvas_left_pixels = g_state.canvas_left_x * 4;
    uint8_t cursor_left_pixels = g_state.cursor_x * g_state.cells_per_pixel * 4;
    POKE(0xD002, SPRITE_OFFSET_X + canvas_left_pixels + cursor_left_pixels);
}

void update_cursor_y() {
    uint8_t yc = g_state.cursor_y * 8;
    POKE(0xD003, SPRITE_OFFSET_Y + (2 * 8) + yc);
}

void update_cursor_xmsb() {
    uint8_t canvas_left_pixels = g_state.canvas_left_x * 4;
    uint8_t cursor_left_pixels = g_state.cursor_x * g_state.cells_per_pixel * 4;
    const unsigned short sx = SPRITE_OFFSET_X + canvas_left_pixels + cursor_left_pixels;
    if (sx < 256) {
        POKE(0xD010, PEEK(0xD010) & ~(1 << EDIT_CURSOR_NUM));
    } else {
        POKE(0xD010, PEEK(0xD010) | (1 << EDIT_CURSOR_NUM));
    }
    POKE(0xD002, (unsigned char)sx);
}

static void draw_shape_char(uint8_t x, uint8_t y) {
    cputncxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel),
        2 + y,
        g_state.cells_per_pixel,
        SHAPE_PREVIEW_CHARACTER);
}

static void draw_line(PaintFunc plot) {
    RECT rc;
    set_effective_tool_rect(&rc);
    if (g_state.tool_org_y == g_state.cursor_y) // Horizontal
    {
        register uint8_t x = rc.left;
        while (x <= rc.right) {
            plot(x++, g_state.cursor_y);
        }
    } else if (g_state.tool_org_x == g_state.cursor_x) // Vertical
    {
        register uint8_t y = rc.top;
        while (y <= rc.bottom) {
            plot(g_state.cursor_x, y++);
        }
    } else // Bresenham- algorithm.
    {
        const signed char dx = rc.right - rc.left;
        const signed char dy = (signed char)-(rc.bottom - rc.top);
        uint8_t x = g_state.tool_org_x, y = g_state.tool_org_y;
        signed char error = dx + dy;
        signed char doubled_error = 0;
        signed char sx = g_state.cursor_x > g_state.tool_org_x ? 1 : -1;
        signed char sy = g_state.cursor_y > g_state.tool_org_y ? 1 : -1;

        for (;;) {
            plot(x, y);
            if (x == g_state.cursor_x && y == g_state.cursor_y) {
                break;
            }
            doubled_error = error * 2;
            if (doubled_error >= dy) {
                error += dy;
                x += sx;
            }
            if (doubled_error <= dx) {
                error += dx;
                y += sy;
            }
        }
    }
    clearattr();
}

/*
static void DrawCircle(PaintFunc plot)
{
  RECT rc;
  SetEffectiveToolRect(&rc);

  // RECT rc;
  // void (*plot)(uint8_t, uint8_t) = bPreview ? DrawShapeChar : g_state.paintCellFn;
  // SetEffectiveToolRect(&rc);

  // const signed char dx = rc.right - rc.left;
  // const signed char dy = -(rc.bottom - rc.top);
  // uint8_t x = g_state.toolOrgX, y = g_state.toolOrgY;
  // signed char error = dx + dy;
  // signed char doubled_error = 0;
  // signed char sx = g_state.cursorX > g_state.toolOrgX ? 1 : -1;
  // signed char sy = g_state.cursorY > g_state.toolOrgY ? 1 : -1;

  // for (;;)
  // {
  //     plot(x, y);
  //     if (x == g_state.cursorX && y == g_state.cursorY)
  //         break;
  //     doubled_error = error * 2;
  //     if (doubled_error >= dy)
  //     {
  //         error += dy;
  //         x += sx;
  //     }
  //     if (doubled_error <= dx)
  //     {
  //         error += dx;
  //         y += sy;
  //     }
  // }
}
*/

static void draw_box(PaintFunc plot) {
    RECT rc;
    register uint8_t x, y, i;
    set_effective_tool_rect(&rc);
    x = rc.left;
    while (x <= rc.right) {
        plot(x, g_state.cursor_y);
        if (g_state.fill_shape) {
            for (i = rc.top + 1; i < rc.bottom; ++i) {
                plot(x, i);
            }
        }
        plot(x++, g_state.tool_org_y);
    }

    y = rc.top;
    while (y <= rc.bottom) {
        plot(g_state.cursor_x, y);
        plot(g_state.tool_org_x, y++);
    }
    clearattr();
}

static void draw_nothing(PaintFunc plot) {
    (void)plot;
}

void set_draw_tool(enum DrawingTool tool) {
    g_state.drawing_tool = tool;
    switch (tool) {
        case DrawingToolBox:
            g_state.fill_shape = 0;
            g_state.draw_shape_fn = draw_box;
            break;
        case DrawingToolFilledBox:
            g_state.fill_shape = 1;
            g_state.draw_shape_fn = draw_box;
            break;
        case DrawingToolLine:
            g_state.draw_shape_fn = draw_line;
            break;
        case DrawingToolPixel:
        default:
            g_state.draw_shape_fn = draw_nothing;
    }
}

static void draw_mono_cell(uint8_t x, uint8_t y) {
    register uint8_t cell = 0;
    const uint8_t row_offset = y * g_state.sprite_width / 8;
    const long byte_addr = (SPRITE_BUFFER + row_offset) + (x / 8);
    const uint8_t pixel = lpeek(byte_addr) & (0x80 >> (x % 8));

    gotoxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel), y + 2);
    for (cell = 0; cell < g_state.cells_per_pixel; ++cell) {
        textcolor(pixel ? g_state.color[ColorFore] : g_state.color[ColorBack]);
        cputc(pixel ? SOLID_BLOCK_CHARACTER : TRANS_CHARACTER);
    }
}

static void draw16_color_cell(uint8_t x, uint8_t y) {
    register uint8_t cell = 0;
    const long byte_addr = (SPRITE_BUFFER + (y * 8)) + (x / 2);
    const uint8_t pixel = 0xF & (lpeek(byte_addr) >> (((x + 1) % 2) * 4));
    // const uint8_t col = (g_state.spriteNumber * 16) + pixel;

    gotoxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel), y + 2);
    for (cell = 0; cell < g_state.cells_per_pixel; ++cell) {
        textcolor(pixel);
        cputc(pixel ? SOLID_BLOCK_CHARACTER : TRANS_CHARACTER);
    }
}

static void draw_multicolor_cell(uint8_t x, uint8_t y) {
    register uint8_t cell = 0;
    const long byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 4)) + (x / 4);
    const uint8_t b = lpeek(byte_addr);
    const uint8_t p0 = b & (0x80 >> (2 * (x % 4)));
    const uint8_t p1 = b & (0x40 >> (2 * (x % 4)));
    uint8_t color = g_state.color[ColorBack];
    if (!p0 && p1) {
        color = g_state.color[ColorMc1];
    } else if (p0 && !p1) {
        color = g_state.color[ColorFore];
    } else if (p0 && p1) {
        color = g_state.color[ColorMc2];
    }

    gotoxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel), y + 2);
    for (cell = 0; cell < g_state.cells_per_pixel; ++cell) {
        textcolor(color);
        cputc(p0 | p1 ? SOLID_BLOCK_CHARACTER : TRANS_CHARACTER);
    }
}

static void paint_pixel_mono(uint8_t x, uint8_t y) {
    const long byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 8)) + (x / 8);
    const uint8_t bitsel = 0x80 >> (x % 8);
    const uint8_t b = lpeek(byte_addr);
    lpoke(byte_addr, g_state.current_color_idx == ColorBack ? (b & ~bitsel) : (b | bitsel));
}

static void paint_pixel_multi(uint8_t x, uint8_t y) {
    const long byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 4)) + (x / 4);
    const uint8_t bitsel = (2 * (x % 4));
    const uint8_t mask = ((0x80 >> bitsel) | (0x40 >> bitsel));
    if (g_state.current_color_idx == ColorBack) {
        lpoke(byte_addr, lpeek(byte_addr) & ~mask);
    } else {
        if (g_state.current_color_idx == ColorFore) {
            lpoke(byte_addr, (lpeek(byte_addr) & ~mask) | (0x80 >> bitsel));
        } else if (g_state.current_color_idx == ColorMc1) {
            lpoke(byte_addr, (lpeek(byte_addr) & ~mask) | (0x40 >> bitsel));
        } else if (g_state.current_color_idx == ColorMc2) {
            lpoke(byte_addr, (lpeek(byte_addr) & ~mask) | ((0x80 >> bitsel) | (0x40 >> bitsel)));
        }
    }
}

static void paint_pixel16_color(uint8_t x, uint8_t y) {
    const long byte_addr = (SPRITE_BUFFER + (y * 8)) + (x / 2);
    const uint8_t bitsel = (((x + 1) % 2) * 4);
    lpoke(byte_addr,
        (uint8_t)((lpeek(byte_addr) & (0xF0 >> bitsel)) |
            (g_state.color[g_state.current_color_idx] << bitsel)));
}

static void clear_sprite() {
    set_redraw_full_canvas();
    lfill(SPRITE_BUFFER, 0, g_state.sprite_size_bytes);
}

static void fetch_vic2_regs_from_slot() {
    // H/Y expand

    const uint8_t spr_bit = 1 << PREVIEW_SPRITE_NUM;
    if (is_sprite_hexpand(g_state.sprite_number)) {
        POKE(0xD01D, PEEK(0xD01D) | spr_bit);
    } else {
        POKE(0xD01D, PEEK(0xD01D) & ~spr_bit);
    }
    if (is_sprite_vexpand(g_state.sprite_number)) {
        POKE(0xD017, PEEK(0xD017) | spr_bit);
    } else {
        POKE(0xD017, PEEK(0xD017) & ~spr_bit);
    }
    if (is_sprite_xwidth(g_state.sprite_number)) {
        POKE(0xD057, PEEK(0xD057) | spr_bit);
    } else {
        POKE(0xD057, PEEK(0xD057) & ~spr_bit);
    }
}

static void fetch_sprite_data_from_slot() {
    // TODO: Sprites may exceed 512 bytes
    freeze_fetch_sector_partial(g_state.sprite_data_addr, SPRITE_BUFFER, g_state.sprite_size_bytes);
}

static void put_sprite_data_to_slot() {
    // TODO: Sprites may exceed 512 bytes
    freeze_store_sector_partial(g_state.sprite_data_addr, SPRITE_BUFFER, g_state.sprite_size_bytes);
}

static void copy_sprite_data(const uint32_t to_addr) {
    // TODO: Sprites may exceed 512 bytes
    freeze_store_sector_partial(to_addr, SPRITE_BUFFER, g_state.sprite_size_bytes);
}

/* An expanded sprite covers twice the pixels, which is what the preview has to
 * centre itself against. */
static inline uint8_t hexpand_factor(uint8_t sprite) {
    return is_sprite_hexpand(sprite) ? 2 : 1;
}
static inline uint8_t vexpand_factor(uint8_t sprite) {
    return is_sprite_vexpand(sprite) ? 2 : 1;
}

static void update_sprite_preview(void) {
    // Setup Preview Area sprite. (we divide by 2 for H320 sprites, should divide by 1 if H640 mode)

    POKE(local_reg_sprite_color(PREVIEW_SPRITE_NUM), g_state.color[ColorFore]);
    POKE(LOCAL_REG_SPRITE_MULTICOL1, g_state.color[ColorMc1]);
    POKE(LOCAL_REG_SPRITE_MULTICOL2, g_state.color[ColorMc2]);

    POKE(0xD004,
        (SPRITE_OFFSET_X +
            ((SIDEBAR_COLUMN * 8 / 2) +
                (((SIDEBAR_WIDTH * 8 / 2) / 2) -
                    (g_state.sprite_width * hexpand_factor(g_state.sprite_number) /
                        (is_sprite_multicolor(g_state.sprite_number) ? 1 : 2))))) &
            0xFF);

    POKE(0xD005,
        (SPRITE_OFFSET_Y + (SIDEBAR_PREVIEW_AREA_TOP * 8) +
            (((SIDEBAR_PREVIEW_AREA_HEIGHT * 8) / 2) -
                (g_state.sprite_height * vexpand_factor(g_state.sprite_number) / 2))));
}

void update_sprite_parameters(bool f_fetch_slot) {
    const bool is_x_width = is_sprite_xwidth(g_state.sprite_number);

    g_state.sprite_height = 21;
    g_state.sprite_size_bytes = sprite_size_bytes(g_state.sprite_number);
    g_state.bytes_per_row = (uint8_t)(g_state.sprite_size_bytes / g_state.sprite_height);
    g_state.sprite_data_addr = sprite_data_addr(g_state.sprite_number);

    if (f_fetch_slot) {
        fetch_sprite_data_from_slot();
        fetch_vic2_regs_from_slot();
    }

    if (is_sprite_16color(g_state.sprite_number)) {
        g_state.draw_cell_fn = draw16_color_cell;
        g_state.paint_cell_fn = paint_pixel16_color;
        g_state.sprite_color_mode = SpriteColorMode16;
        g_state.sprite_width = 16; // Extended width is implied for 16-color sprites.
        g_state.cells_per_pixel = 3;
        g_state.pixels_per_byte = 2;
        g_state.current_color_idx = ColorFore;
        POKE(LOCAL_REG_SPR_16COL, PEEK(LOCAL_REG_SPR_16COL) | (1 << PREVIEW_SPRITE_NUM));
        POKE(LOCAL_REG_SPR_MULTICOLOR, PEEK(LOCAL_REG_SPR_MULTICOLOR) & ~(1 << PREVIEW_SPRITE_NUM));
    } else if (is_sprite_multicolor(g_state.sprite_number)) {
        g_state.draw_cell_fn = draw_multicolor_cell;
        g_state.paint_cell_fn = paint_pixel_multi;
        g_state.sprite_color_mode = SpriteColorModeMulti;
        g_state.sprite_width = is_x_width ? 32 : 12;
        g_state.cells_per_pixel = is_x_width ? 2 : 4;
        g_state.pixels_per_byte = 4;
        g_state.color[ColorFore] = reg_peek(reg_sprite_color(g_state.sprite_number));
        g_state.color[ColorMc1] = reg_peek(REG_SPRITE_MULTICOL1);
        g_state.color[ColorMc2] = reg_peek(REG_SPRITE_MULTICOL2);
        POKE(LOCAL_REG_SPR_16COL, PEEK(LOCAL_REG_SPR_16COL) & ~(1 << PREVIEW_SPRITE_NUM));
        POKE(LOCAL_REG_SPR_MULTICOLOR, PEEK(LOCAL_REG_SPR_MULTICOLOR) | (1 << PREVIEW_SPRITE_NUM));
    } else {
        g_state.draw_cell_fn = draw_mono_cell;
        g_state.paint_cell_fn = paint_pixel_mono;
        g_state.sprite_color_mode = SpriteColorModeMono;
        g_state.sprite_width = is_x_width ? 64 : 24;
        g_state.cells_per_pixel = is_x_width ? 1 : 2;
        g_state.pixels_per_byte = 8;
        g_state.color[ColorFore] = reg_peek(reg_sprite_color(g_state.sprite_number));
        POKE(LOCAL_REG_SPR_16COL, PEEK(LOCAL_REG_SPR_16COL) & ~(1 << PREVIEW_SPRITE_NUM));
        POKE(LOCAL_REG_SPR_MULTICOLOR, PEEK(LOCAL_REG_SPR_MULTICOLOR) & ~(1 << PREVIEW_SPRITE_NUM));
    }

    g_state.cells_per_pixel >>= g_state.wide_screen_mode;
    g_state.canvas_left_x =
        (SIDEBAR_COLUMN / 2) - (g_state.sprite_width * g_state.cells_per_pixel / 2);

    // Restore border affected by previous SD Card I/O
    bordercolor(DEFAULT_BORDER_COLOR);

    update_sprite_preview();

    // Setup Edit cursor

    lcopy((long)EDIT_CURSORS + 63 * (g_state.cells_per_pixel - 1), 0x3C0, 63);

    // The edit cursor maybe off-bounds if a different sprite type was switched,
    // so force to recalculate
    g_state.cursor_x = MIN(g_state.cursor_x, g_state.sprite_width - 1);
    g_state.cursor_y = MIN(g_state.cursor_y, g_state.sprite_height - 1);
    g_state.update_cursor_x_fn();
    g_state.update_cursor_y_fn();
}

static void update_color_regs() {
    reg_poke(reg_sprite_color(g_state.sprite_number), g_state.color[ColorFore]);
    reg_poke(REG_SPRITE_MULTICOL1, g_state.color[ColorMc1]);
    reg_poke(REG_SPRITE_MULTICOL2, g_state.color[ColorMc2]);
    bordercolor(COLOUR_BLUE);

    POKE(local_reg_sprite_color(PREVIEW_SPRITE_NUM), g_state.color[ColorFore]);
    POKE(LOCAL_REG_SPRITE_MULTICOL1, g_state.color[ColorMc1]);
    POKE(LOCAL_REG_SPRITE_MULTICOL2, g_state.color[ColorMc2]);
}

static void erase_canvas_space() {
    RECT rc;
    rc.top = 2;
    rc.left = 0;
    rc.right = SIDEBAR_COLUMN - 1;
    rc.bottom = 23;
    fillrect(&rc, ' ', 1);
}

static void draw_canvas() {
    register uint8_t row;
    register uint8_t col;
    for (row = g_state.redraw_rect.top; row < g_state.redraw_rect.bottom; ++row) {
        for (col = g_state.redraw_rect.left; col < g_state.redraw_rect.right; ++col) {
            g_state.draw_cell_fn(col, row);
        }
    }

    if (g_state.tool_active && (g_state.redraw_flags & RedrawToolPreview)) {
        blink(1);
        revers(1);
        textcolor(g_state.color[g_state.current_color_idx]);
        g_state.draw_shape_fn(draw_shape_char);
    }

    set_rect(&g_state.redraw_rect, 0, 0, 0, 0);
    g_state.redraw_flags &= ~RedrawToolPreview;
}

void set_effective_tool_rect(RECT* rc) {
    set_rect(rc,
        MIN(g_state.tool_org_x, g_state.cursor_x),
        MIN(g_state.tool_org_y, g_state.cursor_y),
        MAX(g_state.tool_org_x, g_state.cursor_x),
        MAX(g_state.tool_org_y, g_state.cursor_y));
}

void set_redraw_full_canvas(void) {
    set_rect(&g_state.redraw_rect, 0, 0, g_state.sprite_width, g_state.sprite_height);
}

/* Exactly 80 columns, so the banner fills row 0 and cannot wrap. */
static const char HEADER_TEXT[] =
    "                            THE MEGA65 SPRITE EDITOR                            ";

static void draw_header() {
    if (g_state.redraw_flags & RedrawHeader) {
        /* Not cprintf.  mega65-libc dispatches its `{...}` escapes through a
         * table indexed by a hash that does not agree with the one filling it,
         * so `{home}`, `{rvson}` and `{lgrn}` reach the wrong handler or none:
         * position and attributes have to be set through the API to be set at
         * all.  cputs writes what cprintf would, since cprintf(s) is
         * _cprintf(0, s) and does not convert. */
        gohome();
        revers(1);
        textcolor(COLOUR_LIGHTGREEN);
        screen_puts(HEADER_TEXT);
        revers(0);
    }
}

static void draw_color_selector() {
    RECT rc;
    if (g_state.redraw_flags & RedrawSidebarColor) {
        set_rect(&rc, SIDEBAR_COLUMN, 5, 80, 7);
        fillrect(&rc, ' ', DEFAULT_SCREEN_COLOR);

        switch (g_state.sprite_color_mode) {
            case SpriteColorModeMono:

                textcolor(g_state.color[ColorBack]);
                screen_putsxy(SIDEBAR_COLUMN, 5, "\xe0\xe0\xe0\xe0\xe0\xe0");
                textcolor(g_state.color[ColorFore]);
                screen_putsxy(SIDEBAR_COLUMN + 8, 5, "\xe0\xe0\xe0\xe0\xe0\xe0");

                textcolor(g_state.current_color_idx == ColorBack ? 1 : COLOUR_DARKGREY);
                screen_putsxy(SIDEBAR_COLUMN + 2, 6, "BK");

                textcolor(g_state.current_color_idx == ColorFore ? 1 : COLOUR_DARKGREY);
                screen_putsxy(SIDEBAR_COLUMN + 8 + 2, 6, "FG");

                break;

            case SpriteColorMode16:
                textcolor(g_state.color[ColorFore]);
                screen_putsxy(SIDEBAR_COLUMN,
                    5,
                    "\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0");

                textcolor(1);
                screen_putsxy(SIDEBAR_COLUMN + 2,
                    6,
                    g_state.color[ColorFore] == 0 ? "BACKGROUND" : "FOREGROUND");
                break;

            case SpriteColorModeMulti:
                textcolor(g_state.color[ColorBack]);
                screen_putsxy(SIDEBAR_COLUMN, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[ColorFore]);
                screen_putsxy(SIDEBAR_COLUMN + 4, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[ColorMc1]);
                screen_putsxy(SIDEBAR_COLUMN + 4 * 2, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[ColorMc2]);
                screen_putsxy(SIDEBAR_COLUMN + 4 * 3, 5, "\xe0\xe0\xe0");

                textcolor(COLOUR_DARKGREY);
                screen_putsxy(SIDEBAR_COLUMN + 1, 6, "BK");
                screen_putsxy(SIDEBAR_COLUMN + 5, 6, "FG");
                screen_putsxy(SIDEBAR_COLUMN + 8, 6, "MC1");
                screen_putsxy(SIDEBAR_COLUMN + 12, 6, "MC2");

                textcolor(1);
                switch (g_state.current_color_idx) {
                    case ColorBack:
                        screen_putsxy(SIDEBAR_COLUMN + 1, 6, "BK");
                        break;
                    case ColorFore:
                        screen_putsxy(SIDEBAR_COLUMN + 5, 6, "FG");
                        break;
                    case ColorMc1:
                        screen_putsxy(SIDEBAR_COLUMN + 8, 6, "MC1");
                        break;
                    case ColorMc2:
                        screen_putsxy(SIDEBAR_COLUMN + 12, 6, "MC2");
                        break;
                    default:
                        break;
                }

                break;
            default:
                break;
        }
    }
}

static void draw_toolbox() {
    register uint8_t i = 0;
    const uint8_t num_buttons = sizeof(CHSET_TOOLBOX) / 8 / 2 / 2;

    if (g_state.redraw_flags & RedrawSidebarTools) {
        for (i = 0; i < num_buttons; ++i) {
            if (g_state.drawing_tool == i) {
                textcolor(COLOUR_WHITE);
            } else {
                textcolor(COLOUR_DARKGREY);
            }

            cputcxy(SIDEBAR_COLUMN + i * 2, SCREEN_ROWS - 4, TOOLBOX_CHARSET_BASE_IDX + i * 2);
            cputcxy(
                SIDEBAR_COLUMN + i * 2 + 1, SCREEN_ROWS - 4, TOOLBOX_CHARSET_BASE_IDX + i * 2 + 1);

            cputcxy(SIDEBAR_COLUMN + i * 2,
                SCREEN_ROWS - 3,
                TOOLBOX_CHARSET_BASE_IDX + i * 2 + num_buttons * 2);
            cputcxy(SIDEBAR_COLUMN + i * 2 + 1,
                SCREEN_ROWS - 3,
                TOOLBOX_CHARSET_BASE_IDX + i * 2 + 1 + num_buttons * 2);
        }
    }
}

static void draw_side_bar_sprite_info() {
    if (g_state.redraw_flags & RedrawSidebarInfo) {
        textcolor(1);
        gotoxy(SIDEBAR_COLUMN, 2);
        screen_puts("SPRITE ");
        cputdec(g_state.sprite_number, 0, 0);
        screen_puts(g_state.sprite_color_mode == SpriteColorModeMono
                ? " MONO    "
                : (g_state.sprite_color_mode == SpriteColorModeMulti ? " MULTI   " : " 16-COL"));
        gotoxy(SIDEBAR_COLUMN, 3);
        textcolor(3);
        cputhex(g_state.sprite_data_addr, 7);
        gotoxy(SIDEBAR_COLUMN, 8);
        textcolor(
            is_sprite_xwidth(g_state.sprite_number) || is_sprite_16color(g_state.sprite_number)
                ? COLOUR_LIGHTBLUE
                : COLOUR_DARKGREY);
        screen_puts("XWIDE");
        textcolor(is_sprite_hexpand(g_state.sprite_number) ? COLOUR_LIGHTBLUE : COLOUR_DARKGREY);
        screen_puts(" HEXP");
        textcolor(is_sprite_vexpand(g_state.sprite_number) ? COLOUR_LIGHTBLUE : COLOUR_DARKGREY);
        screen_puts(" VEXP");
    }
}

static void draw_coordinates() {
    if (g_state.redraw_flags & RedrawSidebarCoords) {
        cputncxy(SIDEBAR_COLUMN, SCREEN_ROWS - 1, SIDEBAR_WIDTH, ' ');
        gotoxy(SIDEBAR_COLUMN, SCREEN_ROWS - 1);
        textcolor(COLOUR_CYAN);
        cputc('(');
        cputdec(g_state.cursor_x, 0, 0);
        cputc(',');
        cputdec(g_state.cursor_y, 0, 0);
        cputc(')');
    }
}

static void draw_sprite_preview_area() {
    register uint8_t i = 0;
    textcolor(g_state.color[ColorBack]);
    for (i = 0; i < SIDEBAR_PREVIEW_AREA_HEIGHT; ++i) {
        cputncxy(
            SIDEBAR_COLUMN, SIDEBAR_PREVIEW_AREA_TOP + i, SIDEBAR_WIDTH, SOLID_BLOCK_CHARACTER);
    }
}

static void draw_sidebar() {
    draw_header();
    draw_side_bar_sprite_info();
    draw_coordinates();
    draw_toolbox();
    draw_color_selector();
    draw_sprite_preview_area();
    g_state.redraw_flags = RedrawNothing;
}

/* Fold the case rather than trust it.  mega65-libc's cinput() upper-cases
 * what it stores on two conditions that do not survive reading: `PEEK(0x0D18)`
 * is a byte of our own program image rather than the $D018 it means, and
 * `flags & ~CINPUT_NO_AUTOTRANSLATE` is true for every flag value rather than
 * false for that one.  So the stored case is a property of the build. */
static bool answered_yes(const uint8_t* answer) {
    return (answer[0] | 0x20) == 'y' && (answer[1] | 0x20) == 'e' && (answer[2] | 0x20) == 's';
}

static void ask(const char* question, uint8_t* into, uint8_t max_length) {
    gotoy(SCREEN_ROWS - 1);
    revers(1);
    textcolor(COLOUR_PINK);
    cputncxy(0, SCREEN_ROWS - 1, SCREEN_COLS, ' ');
    screen_putsxy(0, SCREEN_ROWS - 1, question);
    cinput(into, max_length + 1, CINPUT_ACCEPT_ALL);
    revers(0);
    textcolor(COLOUR_BLUE);
    cputncxy(0, SCREEN_ROWS - 1, SCREEN_COLS, ' ');
    g_state.redraw_flags |= RedrawSidebarCoords;
}

static void print_key_group(const char* list[], uint8_t count, uint8_t x, uint8_t y) {
    register uint8_t i = 0;
    gotoxy(x, y);
    revers(1);
    textcolor(COLOUR_PINK);
    screen_puts(list[0]);
    revers(0);
    textcolor(COLOUR_WHITE);

    for (i = 1; i < count; ++i) {
        gotoxy(x, y + i);
        screen_puts(list[i]);
    }
}

static void show_help() {
    // clang-format off
  static const char* file_keys[] = {
    "  FILE / TXFER     ",
    "(NOT IMPL)       F5", // load
    "(NOT IMPL)     F7,R", // save raw
    "(NOT IMPL)     F7,B", // save basic
    "FETCH SLOT       F9",
    "STORE SLOT      F11",
    "EXIT             F3",
  };

  static const char* draw_keys[] = {
    "       TOOLS       ",
    "PIXEL             P",
    "LINE              L",
    "BOX               X",
    "FILLED BOX      S-X",
    "(NOT IMPL)        O", // circle
    "(NOT IMPL)      S-O", // filled circle
  };

  static const char* color_keys[] = {
    "       COLOR       ",
    "SELECT         0..9",
    "               A..F",
    "PREV COMPONENT    -",
    "NEXT COMPONENT    +",
    "SELECT BACKGROUND K",
    "SEL PAL BANK CTRL+P",
  };

  static const char* edit_keys[] = {
    "       EDIT        ",
    "SPACEBAR       DRAW",
    "DEL           ERASE",
    "CLEAR        CTRL+N",
    "PREV SPRITE       <",
    "NEXT SPRITE       >",
    "CHANGE TYPE       *",
    "TOGGLE H-EXPAND   H",
    "TOGGLE V-EXPAND   V",
    "(NOT IMPL)        \x1E", // toggle x-width
    "COPY SPRITE  CTRL+C",
    "(NOT IMPL)   CTRL+H", // horz flip
    "(NOT IMPL)   CTRL+V", // vert flip
  };

  static const char* display_keys[] = {
    "     DISPLAY       ",
    "ASPECT RATIO  ALT+R",
    "(NOT IMPL)    ALT+D", // 25/50-line
  };

  static const char* tips_ntricks[] = {
    "   TIPS & TRICKS   ",
    "PRESS F11 TO STORE ",
    "CURRENT SPRITE     ",
    "BEFORE EXITING.    ",
  };
    // clang-format on

    VICIV.spr_ena = 0;

    flushkeybuf();
    clrscr();
    g_state.redraw_flags |= RedrawHeader;
    draw_header();
    print_key_group(file_keys, ARRAY_SIZE(file_keys), 0, 2);
    print_key_group(edit_keys, ARRAY_SIZE(edit_keys), 22, 2);
    print_key_group(draw_keys, ARRAY_SIZE(draw_keys), 0, 2 + ARRAY_SIZE(file_keys) + 1);
    print_key_group(color_keys,
        ARRAY_SIZE(color_keys),
        0,
        2 + ARRAY_SIZE(file_keys) + 1 + ARRAY_SIZE(draw_keys) + 1);
    print_key_group(display_keys, ARRAY_SIZE(display_keys), 42, 2);
    print_key_group(tips_ntricks, ARRAY_SIZE(tips_ntricks), 42, 16);

    textcolor(COLOUR_CYAN);
    revers(1);
    cputncxy(22, SCREEN_ROWS - 4, SCREEN_COLS - 22, 32);
    screen_putsxy(
        22, SCREEN_ROWS - 3, " MEGA65 SPRITE EDITOR  V0.10 (C) 2021 HERNAN DI PIETRO    ");
    screen_putsxy(
        22, SCREEN_ROWS - 2, " MOUSE/JOY CODE BY PAUL GARDNER-STEPHEN.                  ");
    cputncxy(22, SCREEN_ROWS - 1, SCREEN_COLS - 22, 32);
    revers(0);

    cgetc();
    clrscr();
    VICIV.spr_ena = 7;
}

static void do_exit() {
    textcolor(14);
    bordercolor(14);
    bgcolor(6);
    clrscr();
}

static void draw_screen() {
    bgcolor(DEFAULT_SCREEN_COLOR);
    clrscr();
    draw_header();
    draw_canvas();
    draw_sidebar();
}

static void update_and_full_redraw(bool f_fetch_slot) {
    update_sprite_parameters(f_fetch_slot);
    erase_canvas_space();
    set_redraw_full_canvas();
}

unsigned short joy_delay_countdown = 0;
unsigned char fire_lock = 0;

unsigned short mx, my;

static void set_background() {
    g_state.redraw_flags = RedrawSidebarColor;
    if (g_state.sprite_color_mode == SpriteColorMode16) {
        g_state.color[g_state.current_color_idx] = 0;
    } else {
        g_state.current_color_idx = ColorBack;
    }
}

/* The edit cursor's colour steps on the raster rather than once per pass of
 * the loop, because the pass rate is whatever the build happens to achieve --
 * here fast enough to recolour the sprite several times within one frame, and
 * mid-frame at that, which shimmers rather than pulses.  A tick is one wrap of
 * the raster line, about 1/100 s on PAL, so sixteen colours take a little
 * under a second. */
constexpr uint8_t CURSOR_COLOUR_TICKS = 6;

static void main_loop() {
    static uint8_t edit_color_index = 0;
    static uint8_t colour_countdown = CURSOR_COLOUR_TICKS;
    static uint8_t previous_raster = 0;
    unsigned char buf[64];
    unsigned char key = 0;

    mouse_set_bounding_box(0 + 24, 0 + 50, 319 + 24 - 8, 199 + 50);
    mouse_warp_to(24, 100);
    mouse_bind_to_sprite(0);

    while (1) {
        const uint8_t raster = VICIV.rasterline;
        if (raster < previous_raster && --colour_countdown == 0) {
            colour_countdown = CURSOR_COLOUR_TICKS;
            VICIV.spr_color[EDIT_CURSOR_NUM] = EDIT_CURSOR_COLOR_MAP[edit_color_index++ & 0x0F];
        }
        previous_raster = raster;

        mouse_update_position(&mx, &my);
        if ((my >= 66 && my <= 233) && (mx >= 55 && mx <= 235)) {
            if ((((mx - 55) / 8) != g_state.cursor_x) || (((my - 66) / 8) != g_state.cursor_y)) {
                g_state.draw_cell_fn(g_state.cursor_x, g_state.cursor_y);
                g_state.cursor_x = (uint8_t)((mx - 55) / 8);
                g_state.cursor_y = (uint8_t)((my - 66) / 8);
                g_state.update_cursor_x_fn();
                g_state.update_cursor_y_fn();
                fire_lock = 0;
            }
        }
        if (kbhit()) {
            key = cgetc();
            joy_delay_countdown = 0;
        } else {
            key = 0;
            if ((CIA1.pra & 0x1f) != 0x1f) {
                // Check joysticks

                if (!(CIA1.pra & 0x10)) {
                    // Toggle pixel
                    if (!fire_lock) {
                        key = 0x20;
                        joy_delay_countdown = joy_delay_countdown >> 3;
                    }
                    fire_lock = 1;
                } else {
                    fire_lock = 0;
                }

                if (joy_delay_countdown) {
                    joy_delay_countdown--;
                } else {
                    switch (CIA1.pra & 0xf) {
                        case 0x7: // RIGHT
                            joy_delay_countdown = JOY_DELAY;
                            fire_lock = 0;
                            key = CH_CURS_RIGHT;
                            break;
                        case 0xB: // LEFT
                            fire_lock = 0;
                            joy_delay_countdown = JOY_DELAY;
                            key = CH_CURS_LEFT;
                            break;
                        case 0xE: // UP
                            fire_lock = 0;
                            joy_delay_countdown = JOY_DELAY;
                            key = CH_CURS_UP;
                            break;
                        case 0xD: // DOWN
                            fire_lock = 0;
                            joy_delay_countdown = JOY_DELAY;
                            key = CH_CURS_DOWN;
                            break;
                        default:
                            key = 0;
                    }
                }
            }
        }
        if (!key) {
            if (mouse_clicked()) {
                if (!fire_lock) {
                    key = 0x20;
                }
                fire_lock = 1;
            }
        }
        switch (key) {
            case 0:
                // No key, do nothing
                break;

                /* ------------------------------------ HELP ----------------------------------- */

            case 31: // HELP
                show_help();
                erase_canvas_space();
                set_redraw_full_canvas();
                textcolor(COLOUR_BLUE);
                cputncxy(0, SCREEN_ROWS - 1, SCREEN_COLS, ' ');
                g_state.redraw_flags = RedrawAll;
                break;

                /* ------------------------- CURSOR MOVEMENT GROUP ----------------------------- */

            case CH_CURS_DOWN:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_y =
                    (g_state.cursor_y == g_state.sprite_height - 1) ? 0 : (g_state.cursor_y + 1);
                g_state.redraw_flags = RedrawSidebarCoords | RedrawToolPreview;
                g_state.update_cursor_y_fn();
                break;

            case CH_CURS_UP:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_y =
                    (g_state.cursor_y == 0) ? (g_state.sprite_height - 1) : (g_state.cursor_y - 1);
                g_state.redraw_flags = RedrawSidebarCoords | RedrawToolPreview;
                g_state.update_cursor_y_fn();
                break;

            case CH_CURS_LEFT:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_x =
                    (g_state.cursor_x == 0) ? (g_state.sprite_width - 1) : (g_state.cursor_x - 1);
                g_state.redraw_flags = RedrawSidebarCoords | RedrawToolPreview;
                g_state.update_cursor_x_fn();
                break;

            case CH_CURS_RIGHT:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_x =
                    (g_state.cursor_x == g_state.sprite_width - 1) ? 0 : (g_state.cursor_x + 1);
                g_state.redraw_flags = RedrawSidebarCoords | RedrawToolPreview;
                g_state.update_cursor_x_fn();
                break;

                /* ------------------------- EDIT GROUP ------------------------------------------
                 */

            case ' ':

                if (g_state.drawing_tool == DrawingToolPixel) {
                    set_rect(&g_state.redraw_rect,
                        g_state.cursor_x,
                        g_state.cursor_y,
                        g_state.cursor_x + 1,
                        g_state.cursor_y + 1);
                    g_state.paint_cell_fn(g_state.cursor_x, g_state.cursor_y);
                } else {
                    if (g_state.tool_active) {
                        g_state.tool_active = 0;
                        g_state.draw_shape_fn(g_state.paint_cell_fn);
                        set_redraw_full_canvas();
                    } else {
                        g_state.tool_org_x = g_state.cursor_x;
                        g_state.tool_org_y = g_state.cursor_y;
                        g_state.tool_active = 1;
                    }
                }
                break;

            case 20: // DEL
            {
                const enum ColorIndex c_index = g_state.current_color_idx;
                set_rect(&g_state.redraw_rect,
                    g_state.cursor_x,
                    g_state.cursor_y,
                    g_state.cursor_x + 1,
                    g_state.cursor_y + 1);
                set_background();
                g_state.paint_cell_fn(g_state.cursor_x, g_state.cursor_y);
                g_state.current_color_idx = c_index;
            }

            break;

            case ',':
            case '.':

                put_sprite_data_to_slot();
                g_state.redraw_flags = RedrawAll;

                if (key == '.' && g_state.sprite_number++ == SPRITE_MAX_COUNT - 1) {
                    g_state.sprite_number = 0;
                } else if (key == ',' && g_state.sprite_number-- == 0) {
                    g_state.sprite_number = SPRITE_MAX_COUNT - 1;
                }

                update_and_full_redraw(true);
                g_state.cursor_x = MIN(g_state.sprite_width - 1, g_state.cursor_x);
                g_state.cursor_y = MIN(g_state.sprite_height - 1, g_state.cursor_y);

                break;

            case '*':
                g_state.tool_active = 0;
                g_state.redraw_flags = RedrawAll;
                switch (g_state.sprite_color_mode) {
                    case SpriteColorMode16:
                        // Switch to Hi-Res
                        reg_poke(REG_SPR_16COL,
                            (uint8_t)(reg_peek(REG_SPR_16COL) & ~(1 << g_state.sprite_number)));
                        reg_poke(REG_SPR_MULTICOLOR,
                            (uint8_t)(reg_peek(REG_SPR_MULTICOLOR) &
                                ~(1 << g_state.sprite_number)));
                        break;
                    case SpriteColorModeMulti:
                        // Switch to 16-col
                        reg_poke(REG_SPR_16COL,
                            (uint8_t)(reg_peek(REG_SPR_16COL) | (1 << g_state.sprite_number)));
                        reg_poke(REG_SPR_MULTICOLOR,
                            (uint8_t)(reg_peek(REG_SPR_MULTICOLOR) &
                                ~(1 << g_state.sprite_number)));
                        break;
                    case SpriteColorModeMono:
                        // Switch to Multicol
                        reg_poke(REG_SPR_16COL,
                            (uint8_t)(reg_peek(REG_SPR_16COL) & ~(1 << g_state.sprite_number)));
                        reg_poke(REG_SPR_MULTICOLOR,
                            (uint8_t)(reg_peek(REG_SPR_MULTICOLOR) | (1 << g_state.sprite_number)));
                        break;
                    default:
                        break;
                }
                update_and_full_redraw(false);
                break;

            case '@': // 94: // Upixel-arrow
                // TODO: Disabled until we can fix it; issue #77
                // POKE(LOCAL_REG_SPRX64EN, PEEK(LOCAL_REG_SPRX64EN) ^ (1 << PREVIEW_SPRITE_NUM));
                // reg_poke(REG_SPRX64EN, reg_peek(REG_SPRX64EN) ^ (1 <<
                // g_state.spriteNumber)); g_state.redrawFlags = RedrawAll;
                // g_state.updateCursorXFn = PEEK(LOCAL_REG_SPRX64EN) & (1 << PREVIEW_SPRITE_NUM) ?
                // UpdateCursorXMSB : UpdateCursorX; UpdateAndFullRedraw(false);
                // UpdateSpritePreview();
                break;

            case 3: // CTRL-C
                ask("COPY SPRITE TO (0-7)? ", buf, 1);
                if (buf[0] >= '0' && buf[0] <= '7') {
                    uint8_t to_sprite = buf[0] - 48;
                    if (sprite_size_bytes(to_sprite) == g_state.sprite_size_bytes) {
                        // Display error if bytes do not match
                        copy_sprite_data(sprite_data_addr(to_sprite));
                    } else {
                        bordercolor(COLOUR_RED);
                        cgetc();
                    }
                    bordercolor(DEFAULT_BORDER_COLOR);
                }
                break;

            case 118: // "V"-expand
                POKE(0xD017, PEEK(0xD017) ^ (1 << PREVIEW_SPRITE_NUM));
                reg_poke(REG_SPR_VEXPAND,
                    (uint8_t)(reg_peek(REG_SPR_VEXPAND) ^ (1 << g_state.sprite_number)));
                bordercolor(DEFAULT_BORDER_COLOR);
                update_sprite_preview();
                g_state.redraw_flags = RedrawSidebarInfo;
                break;

            case 104: // "H"-expand
                POKE(0xD01D, PEEK(0xD01D) ^ (1 << PREVIEW_SPRITE_NUM));
                reg_poke(REG_SPR_HEXPAND,
                    (uint8_t)(reg_peek(REG_SPR_HEXPAND) ^ (1 << g_state.sprite_number)));
                bordercolor(DEFAULT_BORDER_COLOR);
                update_sprite_preview();
                g_state.redraw_flags = RedrawSidebarInfo;
                break;

            case 8:  // CTRL-H, horizontal flip
            case 22: // CTRL-V, vertical flip
                // TODO: implement; issue #78
                break;

                /* ------------------------------- COLOR GROUP -------------------------- */

            case '+':
                g_state.redraw_flags = RedrawSidebarColor;
                g_state.current_color_idx = (g_state.current_color_idx + 1) %
                    ((g_state.sprite_color_mode == SpriteColorModeMono |
                         g_state.sprite_color_mode == SpriteColorMode16)
                            ? 2
                            : 4);
                break;

            case '-':
                g_state.redraw_flags = RedrawSidebarColor;
                g_state.current_color_idx = (g_state.current_color_idx - 1) %
                    ((g_state.sprite_color_mode == SpriteColorModeMono |
                         g_state.sprite_color_mode == SpriteColorMode16)
                            ? 2
                            : 4);
                break;

            case 107: // k
                set_background();
                break;

            case 16: // Ctrl+P
            {
                const unsigned char c_bank_reg = reg_peek(REG_SPRPALSEL);
                const unsigned char c_spr_pal_bank = (c_bank_reg & 0xC) >> 2;
                gotoxy(1, 1);
                cputdec(c_spr_pal_bank, 0, 4);
                reg_poke(REG_SPRPALSEL,
                    (uint8_t)((c_bank_reg & ~0xC) | (((c_spr_pal_bank + 1) % 4) << 2)));
                bordercolor(DEFAULT_BORDER_COLOR);
                setup_text_palette();
            } break;

                /* ------------------------- DISPLAY GROUP ---------------------------------------
                 */

            case 240: // ALT-D
                // TODO: Disabled until we can fix it; issue #74
                // setscreensize(80, 50);
                // UpdateAndFullRedraw(false);
                break;

            case 174: // ALT-R
                g_state.wide_screen_mode = ~g_state.wide_screen_mode & 1;
                update_and_full_redraw(false);
                break;

                /* --------------------------- FILE GROUP ----------------------------- */

            case 0xF3: // F3
                ask("EXIT SPRITE EDITOR: ARE YOU SURE (YES/NO)? ", buf, 3);
                if (answered_yes(buf)) {
                    return;
                }
                break;

            case 0xF5: // F5, load
            case 0xF7: // F7, save
                // TODO: implement
                break;

            case 0xF9: // F9 Fetch from slot
                update_and_full_redraw(true);
                break;

            case 0xFB: // F11 Save to slot
                put_sprite_data_to_slot();
                bordercolor(DEFAULT_BORDER_COLOR);
                break;

            case 14: // CTRL-N
                ask("CLEAR SPRITE (YES/NO)? ", buf, 3);
                if (answered_yes(buf)) {
                    clear_sprite();
                }
                break;

                // case ASC_HELP:
                //     break;

                /* --------------------------- DRAWING TOOLS GROUP ----------------------- */

                /*
                case 111: // o = circle  tool
                  SetDrawTool(DrawingToolCircle);
                  g_state.redrawFlags = RedrawSidebarTools | RedrawToolPreview;

                  SetRedrawFullCanvas();
                  break;

                case 79: // "O" = filled circle  tool
                  SetDrawTool(DrawingToolFilledCircle);
                  g_state.redrawFlags = RedrawSidebarTools | RedrawToolPreview;
                  SetRedrawFullCanvas();
                  break;
                */

            case 112: // pixel=pixel tool
                set_draw_tool(DrawingToolPixel);
                g_state.redraw_flags = RedrawSidebarTools | RedrawToolPreview;
                set_redraw_full_canvas();
                g_state.tool_active = 0;
                break;

            case 120: // x = draw box
                set_draw_tool(DrawingToolBox);
                g_state.redraw_flags = RedrawSidebarTools | RedrawToolPreview;
                set_redraw_full_canvas();
                break;

            case 88: // "X"
                set_draw_tool(DrawingToolFilledBox);
                g_state.redraw_flags = RedrawSidebarTools | RedrawToolPreview;
                set_redraw_full_canvas();
                break;

            case 108: // l = line
                set_draw_tool(DrawingToolLine);
                g_state.redraw_flags = RedrawSidebarTools | RedrawToolPreview;
                set_redraw_full_canvas();
                break;

            default:
                if (key >= '0' && key <= '9') {
                    g_state.color[g_state.current_color_idx] = key - 48;
                    g_state.redraw_flags = RedrawSidebarColor | RedrawToolPreview;
                    set_redraw_full_canvas();
                    update_color_regs();
                } else if (key >= 97 && key <= 102) // a..f
                {
                    g_state.color[g_state.current_color_idx] = 10 + key - 'a';
                    g_state.redraw_flags = RedrawSidebarColor | RedrawToolPreview;
                    set_redraw_full_canvas();
                    update_color_regs();
                }
        }
        draw_canvas();
        draw_sidebar();
    }
}

void do_sprite_editor() {
    initialize();
    draw_screen();
    main_loop();
    do_exit();
}
