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

 */
#include "colours.h"
#include "dma.h"
#include "lineedit.h"
#include "mega65_regs.h"
#include "screen.h"
#include "slot.h"
#include "textout.h"

#include <mega65.h>
#include <mega65/hal.h>
#include <mega65/mouse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #define SPRITED_STANDALONE

/* The sprite registers being edited, and the pair that reaches them.  Built
 * into the freezer they are the frozen program's registers, which live in the
 * freeze slot on the SD card rather than in memory: reg_peek() goes out
 * through freeze_peek()'s sector I/O, which is why VIC_BASE cannot be the
 * address of an MMIO struct however much it looks like one.  The editor's own
 * display is a different thing entirely and goes through VICIV. */
#ifdef SPRITED_STANDALONE
constexpr uint32_t VIC_BASE = 0xD000UL;
constexpr uint32_t CIA2_PORT_A = 0xDD00UL;

static inline uint8_t reg_peek(uint32_t address) {
    return REG8(address);
}
static inline void reg_poke(uint32_t address, uint8_t value) {
    REG8(address) = value;
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

constexpr uint8_t SPRITE_MAX_COUNT = 8;
/* A sprite colour, not chrome: it stands in for the frozen VIC's own value
 * until one is read.  The index is fixed, but entry 11 belongs to the scheme,
 * so what it draws as still follows the palette. */
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

/* The canvas starts below the banner and the sprite-info row. */
constexpr uint8_t CANVAS_TOP_ROW = 2;

/* H320/V200, the space the mouse and the sprites share. */
constexpr uint16_t SCREEN_MAX_X = 319;
constexpr uint16_t SCREEN_MAX_Y = 199;
constexpr uint8_t POINTER_WIDTH = 8;
constexpr uint8_t POINTER_START_X = 100;
constexpr uint8_t POINTER_START_Y = 100;

/* The canvas in mouse coordinates, which are sprite pixels rather than screen
 * columns and rows. */
constexpr uint16_t CANVAS_MOUSE_LEFT = 55;
constexpr uint16_t CANVAS_MOUSE_RIGHT = 235;
constexpr uint16_t CANVAS_MOUSE_TOP = 66;
constexpr uint16_t CANVAS_MOUSE_BOTTOM = 233;
constexpr uint8_t MOUSE_CELL_PIXELS = 8;
/* A character cell measured in sprite pixels: the text is H640 and the sprites
 * are H320, so a column is four sprite pixels wide, and a row eight high. */
constexpr uint8_t CELL_PIXELS_X = 4;
constexpr uint8_t CELL_PIXELS_Y = 8;

constexpr uint16_t JOY_DELAY = 10000U;

/* These stay macros: they are type-generic, and C has no other spelling of
 * that without _Generic. */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

constexpr uint32_t SPRITE_POINTER_TABLE = 0x16000UL;
constexpr uint32_t SPRITE_BUFFER = 0x40000UL;

/* The editor's own three sprites, in the live VIC.  These index VICIV's
 * spr_pos[], spr_color[] and the pointer table, so they name the sprite
 * everywhere rather than hiding it in a register address. */
constexpr uint8_t MOUSE_POINTER_NUM = 0;
constexpr uint8_t EDIT_CURSOR_NUM = 1;
constexpr uint8_t PREVIEW_SPRITE_NUM = 2;
constexpr uint8_t EDITOR_SPRITES =
    (1 << MOUSE_POINTER_NUM) | (1 << EDIT_CURSOR_NUM) | (1 << PREVIEW_SPRITE_NUM);

/* A sprite pointer is an address divided by 64, so the alignment below is
 * load-bearing: without it the division truncates and the VIC-IV reads the
 * wrong data.  Nothing else constrains where these sit -- they are never handed
 * to another tool -- so the compiler places them, as SPRITE_BUFFER at $40000
 * already shows sprite data need not be in low memory. */
static uint8_t mouse_pointer[64] __attribute__((aligned(64)));
#define MOUSE_POINTER_DATA ((Addr28)(uint16_t)mouse_pointer)
static uint8_t sprite_cursor[64] __attribute__((aligned(64)));
#define EDIT_CURSOR_DATA ((Addr28)(uint16_t)sprite_cursor)

/* A hires sprite is 24x21 pixels, three bytes to the row. */
constexpr uint8_t SPRITE_ROWS = 21;
constexpr uint8_t SPRITE_FRAME_BYTES = 63;
/* Sprite data addresses are stored divided by this, in the pointer table and
 * in the pointers the frozen program keeps. */
constexpr uint8_t SPRITE_BLOCK_BYTES = 64;
/* Bit 7 of SPRPTR2: pointers are 16-bit words rather than single bytes. */
constexpr uint8_t SPRITE_POINTERS_16BIT = 0x80;

/* The ROM character set, as the VIC-IV sees it in the upper memory map. */
/* CHARSET A, the same set the other tools render through the ROM shadow.
 * Its second bank is the lowercase one, where letters sit at their ASCII
 * values; this is the first, in screen-code order. */
constexpr uint32_t ROM_CHARSET_SOURCE = 0x29000;
constexpr uint16_t CHARSET_BYTES = 2048;
constexpr uint8_t GLYPH_BYTES = 8;

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

constexpr uint8_t TOOLBOX_CHARSET_BASE_IDX = 232;

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

static void set_rect(RECT* rc, uint8_t left, uint8_t top, uint8_t right, uint8_t bottom) {
    rc->left = left;
    rc->right = right;
    rc->top = top;
    rc->bottom = bottom;
}

/* SPRPTR16 is enabled below, so a table entry is a 16-bit word holding the
 * data address divided by 64. */
static void set_sprite_pointer(uint8_t sprite, uint32_t data_address) {
    const uint16_t block = (uint16_t)(data_address / SPRITE_BLOCK_BYTES);
    lpoke(SPRITE_POINTER_TABLE + sprite * 2, (uint8_t)block);
    lpoke(SPRITE_POINTER_TABLE + sprite * 2 + 1, (uint8_t)(block >> 8));
}

static void initialize(void) {
    CPU_PORTDDR = CPU_PORT_FORCE_FAST;

    // --- Freezer slot setup

    freeze_slot_start_sector = read_freeze_slot_start_sector(0);

    request_freeze_region_list();

    // --- Screen setup ----

    sprited_screen_init();
    bordercolor(SchemeBorder);
    bgcolor(SchemeBackground);

    // --- Charset setup ----

    lcopy(ROM_CHARSET_SOURCE, EDITOR_CHARSET, CHARSET_BYTES);
    lcopy((Addr28)CHSET_TOOLBOX,
        EDITOR_CHARSET + TOOLBOX_CHARSET_BASE_IDX * GLYPH_BYTES,
        sizeof(CHSET_TOOLBOX));

    // --- Sprite setup ----

    VICIV.spr_ptradr_lsb = (uint8_t)SPRITE_POINTER_TABLE;
    VICIV.spr_ptradr_msb = (uint8_t)(SPRITE_POINTER_TABLE >> 8);
    VICIV.spr_ptradr_bnk = (uint8_t)(SPRITE_POINTER_TABLE >> 16) | SPRITE_POINTERS_16BIT;

    lcopy((Addr28)SPRITE_POINTER, MOUSE_POINTER_DATA, SPRITE_FRAME_BYTES);
    set_sprite_pointer(MOUSE_POINTER_NUM, MOUSE_POINTER_DATA);
    set_sprite_pointer(EDIT_CURSOR_NUM, EDIT_CURSOR_DATA);
    /* The preview shows the sprite being edited, straight out of the buffer. */
    set_sprite_pointer(PREVIEW_SPRITE_NUM, SPRITE_BUFFER);

    // Sprite properties (color, initial pos, etc.)

    VICIV.spr_enalpha = 0; // Alpha OFF
    VICIV.spr_env400 = 0;  // V400 mode off for editor sprites.
    VICIV.spr_ymsbs = 0;   // Y-MSBs off
    VICIV.spr_ysmsbs = 0;  // Y-MSBs off

    VICIV.spr_ena = EDITOR_SPRITES;
    VICIV.spr_exp_x = 0; // H-expand off for editor sprites.
    VICIV.spr_exp_y = 0; // V-expand off for editor sprites.
    VICIV.spr_pos[MOUSE_POINTER_NUM].x = POINTER_START_X;
    VICIV.spr_pos[MOUSE_POINTER_NUM].y = POINTER_START_Y;

    VICIV.spr_pos[EDIT_CURSOR_NUM].x = 0;
    VICIV.spr_pos[EDIT_CURSOR_NUM].y = 0;

    VICIV.spr_color[MOUSE_POINTER_NUM] = SchemePointer;
    VICIV.spr_color[EDIT_CURSOR_NUM] = SchemeCursor;
    /* The preview sits in the sidebar, past X=255. */
    VICIV.spr_hi_x = 1 << PREVIEW_SPRITE_NUM;
    VICIV.spr_mcolor = 0; // All mono/hires sprites
    VICIV.spr_16en = 0;   // 16-color mode OFF

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

void load_slot_sprite_palette(void) {
}

void setup_text_palette(void) {
    // To properly display color in the editor, we set the main palette bank to 0,
    // the sprite palette bank, and use the alt palette to display text and UI.
    // Werror do this because ALTPAL is selected by VIC-III HIGHLIGHT+UNDERLINE
    // combination and  Foreground colors in alt-palette are used from the 16th index,
    // so we avoid fiddling with this.
}

void update_cursor_x(void) {
    uint8_t canvas_left_pixels = g_state.canvas_left_x * CELL_PIXELS_X;
    uint8_t cursor_left_pixels = g_state.cursor_x * g_state.cells_per_pixel * CELL_PIXELS_X;
    VICIV.spr_pos[EDIT_CURSOR_NUM].x = SPRITE_OFFSET_X + canvas_left_pixels + cursor_left_pixels;
}

void update_cursor_y(void) {
    uint8_t yc = g_state.cursor_y * CELL_PIXELS_Y;
    VICIV.spr_pos[EDIT_CURSOR_NUM].y = SPRITE_OFFSET_Y + (CANVAS_TOP_ROW * CELL_PIXELS_Y) + yc;
}

void update_cursor_xmsb(void) {
    uint8_t canvas_left_pixels = g_state.canvas_left_x * CELL_PIXELS_X;
    uint8_t cursor_left_pixels = g_state.cursor_x * g_state.cells_per_pixel * CELL_PIXELS_X;
    const uint16_t sx = SPRITE_OFFSET_X + canvas_left_pixels + cursor_left_pixels;
    if (sx < 256) {
        VICIV.spr_hi_x &= (uint8_t)~(1 << EDIT_CURSOR_NUM);
    } else {
        VICIV.spr_hi_x |= 1 << EDIT_CURSOR_NUM;
    }
    VICIV.spr_pos[EDIT_CURSOR_NUM].x = (unsigned char)sx;
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
        uint8_t x = g_state.tool_org_x;
        uint8_t y = g_state.tool_org_y;
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

static void draw_box(PaintFunc plot) {
    RECT rc;
    register uint8_t x;
    register uint8_t y;
    register uint8_t i;
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
    const uint32_t byte_addr = (SPRITE_BUFFER + row_offset) + (x / 8);
    const uint8_t pixel = lpeek(byte_addr) & (0x80 >> (x % 8));

    gotoxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel), y + 2);
    /* The colour belongs to the pixel, not the cell: one sprite pixel is up to
     * four cells, and the canvas redraws over a thousand of them. */
    textcolor(pixel ? g_state.color[ColorFore] : g_state.color[ColorBack]);
    for (cell = 0; cell < g_state.cells_per_pixel; ++cell) {
        cputc(pixel ? SOLID_BLOCK_CHARACTER : TRANS_CHARACTER);
    }
}

static void draw16_color_cell(uint8_t x, uint8_t y) {
    register uint8_t cell = 0;
    const uint32_t byte_addr = (SPRITE_BUFFER + (y * 8)) + (x / 2);
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
    const uint32_t byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 4)) + (x / 4);
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
    const uint32_t byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 8)) + (x / 8);
    const uint8_t bitsel = 0x80 >> (x % 8);
    const uint8_t b = lpeek(byte_addr);
    lpoke(byte_addr, g_state.current_color_idx == ColorBack ? (b & ~bitsel) : (b | bitsel));
}

static void paint_pixel_multi(uint8_t x, uint8_t y) {
    const uint32_t byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 4)) + (x / 4);
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
    const uint32_t byte_addr = (SPRITE_BUFFER + (y * 8)) + (x / 2);
    const uint8_t bitsel = (((x + 1) % 2) * 4);
    lpoke(byte_addr,
        (uint8_t)((lpeek(byte_addr) & (0xF0 >> bitsel)) |
            (g_state.color[g_state.current_color_idx] << bitsel)));
}

static void clear_sprite(void) {
    set_redraw_full_canvas();
    lfill(SPRITE_BUFFER, 0, g_state.sprite_size_bytes);
}

static void fetch_vic2_regs_from_slot(void) {
    // H/Y expand

    const uint8_t spr_bit = 1 << PREVIEW_SPRITE_NUM;
    if (is_sprite_hexpand(g_state.sprite_number)) {
        VICIV.spr_exp_x |= spr_bit;
    } else {
        VICIV.spr_exp_x &= (uint8_t)~spr_bit;
    }
    if (is_sprite_vexpand(g_state.sprite_number)) {
        VICIV.spr_exp_y |= spr_bit;
    } else {
        VICIV.spr_exp_y &= (uint8_t)~spr_bit;
    }
    if (is_sprite_xwidth(g_state.sprite_number)) {
        VICIV.spr_x64en |= spr_bit;
    } else {
        VICIV.spr_x64en &= (uint8_t)~spr_bit;
    }
}

/* The canvas shows the frozen program's sprite, never one of ours: an empty
 * canvas means an empty slot, which is what an emulator started with -prg
 * gives -- nothing was ever frozen for this to read. */
static void fetch_sprite_data_from_slot(void) {
    // TODO: Sprites may exceed 512 bytes
    freeze_fetch_sector_partial(g_state.sprite_data_addr, SPRITE_BUFFER, g_state.sprite_size_bytes);
}

static void put_sprite_data_to_slot(void) {
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

    VICIV.spr_color[PREVIEW_SPRITE_NUM] = g_state.color[ColorFore];
    VICIV.spr_mcolors[0] = g_state.color[ColorMc1];
    VICIV.spr_mcolors[1] = g_state.color[ColorMc2];

    VICIV.spr_pos[PREVIEW_SPRITE_NUM].x = (uint8_t)(SPRITE_OFFSET_X +
        ((SIDEBAR_COLUMN * CELL_PIXELS_X) +
            ((SIDEBAR_WIDTH * CELL_PIXELS_X / 2) -
                (g_state.sprite_width * hexpand_factor(g_state.sprite_number) /
                    (is_sprite_multicolor(g_state.sprite_number) ? 1 : 2)))));

    VICIV.spr_pos[PREVIEW_SPRITE_NUM].y =
        (uint8_t)(SPRITE_OFFSET_Y + (SIDEBAR_PREVIEW_AREA_TOP * CELL_PIXELS_Y) +
            ((SIDEBAR_PREVIEW_AREA_HEIGHT * CELL_PIXELS_Y / 2) -
                (g_state.sprite_height * vexpand_factor(g_state.sprite_number) / 2)));
}

void update_sprite_parameters(bool f_fetch_slot) {
    const bool is_x_width = is_sprite_xwidth(g_state.sprite_number);

    g_state.sprite_height = SPRITE_ROWS;
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
        VICIV.spr_16en |= 1 << PREVIEW_SPRITE_NUM;
        VICIV.spr_mcolor &= (uint8_t)~(1 << PREVIEW_SPRITE_NUM);
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
        VICIV.spr_16en &= (uint8_t)~(1 << PREVIEW_SPRITE_NUM);
        VICIV.spr_mcolor |= 1 << PREVIEW_SPRITE_NUM;
    } else {
        g_state.draw_cell_fn = draw_mono_cell;
        g_state.paint_cell_fn = paint_pixel_mono;
        g_state.sprite_color_mode = SpriteColorModeMono;
        g_state.sprite_width = is_x_width ? 64 : 24;
        g_state.cells_per_pixel = is_x_width ? 1 : 2;
        g_state.pixels_per_byte = 8;
        g_state.color[ColorFore] = reg_peek(reg_sprite_color(g_state.sprite_number));
        VICIV.spr_16en &= (uint8_t)~(1 << PREVIEW_SPRITE_NUM);
        VICIV.spr_mcolor &= (uint8_t)~(1 << PREVIEW_SPRITE_NUM);
    }

    g_state.cells_per_pixel >>= g_state.wide_screen_mode;
    g_state.canvas_left_x =
        (SIDEBAR_COLUMN / 2) - (g_state.sprite_width * g_state.cells_per_pixel / 2);

    // Restore border affected by previous SD Card I/O
    bordercolor(SchemeBorder);

    update_sprite_preview();

    // Setup Edit cursor

    /* One frame per cell width.  Widening after the arithmetic rather than
     * before keeps this a 16-bit add: both the array and the offset are. */
    const uint8_t* cursor_frame = EDIT_CURSORS + SPRITE_FRAME_BYTES * (g_state.cells_per_pixel - 1);
    lcopy((Addr28)cursor_frame, EDIT_CURSOR_DATA, SPRITE_FRAME_BYTES);

    // The edit cursor maybe off-bounds if a different sprite type was switched,
    // so force to recalculate
    g_state.cursor_x = MIN(g_state.cursor_x, g_state.sprite_width - 1);
    g_state.cursor_y = MIN(g_state.cursor_y, g_state.sprite_height - 1);
    g_state.update_cursor_x_fn();
    g_state.update_cursor_y_fn();
}

static void update_color_regs(void) {
    reg_poke(reg_sprite_color(g_state.sprite_number), g_state.color[ColorFore]);
    reg_poke(REG_SPRITE_MULTICOL1, g_state.color[ColorMc1]);
    reg_poke(REG_SPRITE_MULTICOL2, g_state.color[ColorMc2]);
    bordercolor(SchemeBorder);

    VICIV.spr_color[PREVIEW_SPRITE_NUM] = g_state.color[ColorFore];
    VICIV.spr_mcolors[0] = g_state.color[ColorMc1];
    VICIV.spr_mcolors[1] = g_state.color[ColorMc2];
}

static void erase_canvas_space(void) {
    RECT rc;
    rc.top = 2;
    rc.left = 0;
    rc.right = SIDEBAR_COLUMN - 1;
    rc.bottom = 23;
    fillrect(&rc, ' ', SchemeText);
}

static void draw_canvas(void) {
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

static void draw_header(void) {
    if (g_state.redraw_flags & RedrawHeader) {
        /* Not cprintf.  mega65-libc dispatches its `{...}` escapes through a
         * table indexed by a hash that does not agree with the one filling it,
         * so `{home}`, `{rvson}` and `{lgrn}` reach the wrong handler or none:
         * position and attributes have to be set through the API to be set at
         * all.  cputs writes what cprintf would, since cprintf(s) is
         * _cprintf(0, s) and does not convert. */
        gohome();
        revers(1);
        textcolor(SchemeBanner);
        cputs(HEADER_TEXT);
        revers(0);
    }
}

static void draw_color_selector(void) {
    RECT rc;
    if (g_state.redraw_flags & RedrawSidebarColor) {
        set_rect(&rc, SIDEBAR_COLUMN, 5, 80, 7);
        fillrect(&rc, ' ', SchemeBackground);

        switch (g_state.sprite_color_mode) {
            case SpriteColorModeMono:

                textcolor(g_state.color[ColorBack]);
                cputsxy(SIDEBAR_COLUMN, 5, "\xe0\xe0\xe0\xe0\xe0\xe0");
                textcolor(g_state.color[ColorFore]);
                cputsxy(SIDEBAR_COLUMN + 8, 5, "\xe0\xe0\xe0\xe0\xe0\xe0");

                textcolor(
                    g_state.current_color_idx == ColorBack ? SchemeSelected : SchemeUnselected);
                cputsxy(SIDEBAR_COLUMN + 2, 6, "BK");

                textcolor(
                    g_state.current_color_idx == ColorFore ? SchemeSelected : SchemeUnselected);
                cputsxy(SIDEBAR_COLUMN + 8 + 2, 6, "FG");

                break;

            case SpriteColorMode16:
                textcolor(g_state.color[ColorFore]);
                cputsxy(SIDEBAR_COLUMN,
                    5,
                    "\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0");

                textcolor(SchemeText);
                cputsxy(SIDEBAR_COLUMN + 2,
                    6,
                    g_state.color[ColorFore] == 0 ? "BACKGROUND" : "FOREGROUND");
                break;

            case SpriteColorModeMulti:
                textcolor(g_state.color[ColorBack]);
                cputsxy(SIDEBAR_COLUMN, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[ColorFore]);
                cputsxy(SIDEBAR_COLUMN + 4, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[ColorMc1]);
                cputsxy(SIDEBAR_COLUMN + 4 * 2, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[ColorMc2]);
                cputsxy(SIDEBAR_COLUMN + 4 * 3, 5, "\xe0\xe0\xe0");

                textcolor(SchemeUnselected);
                cputsxy(SIDEBAR_COLUMN + 1, 6, "BK");
                cputsxy(SIDEBAR_COLUMN + 5, 6, "FG");
                cputsxy(SIDEBAR_COLUMN + 8, 6, "MC1");
                cputsxy(SIDEBAR_COLUMN + 12, 6, "MC2");

                textcolor(SchemeSelected);
                switch (g_state.current_color_idx) {
                    case ColorBack:
                        cputsxy(SIDEBAR_COLUMN + 1, 6, "BK");
                        break;
                    case ColorFore:
                        cputsxy(SIDEBAR_COLUMN + 5, 6, "FG");
                        break;
                    case ColorMc1:
                        cputsxy(SIDEBAR_COLUMN + 8, 6, "MC1");
                        break;
                    case ColorMc2:
                        cputsxy(SIDEBAR_COLUMN + 12, 6, "MC2");
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

static void draw_toolbox(void) {
    register uint8_t i = 0;
    const uint8_t num_buttons = sizeof(CHSET_TOOLBOX) / 8 / 2 / 2;

    if (g_state.redraw_flags & RedrawSidebarTools) {
        for (i = 0; i < num_buttons; ++i) {
            if (g_state.drawing_tool == i) {
                textcolor(SchemeSelected);
            } else {
                textcolor(SchemeUnselected);
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

static void draw_side_bar_sprite_info(void) {
    if (g_state.redraw_flags & RedrawSidebarInfo) {
        textcolor(SchemeText);
        gotoxy(SIDEBAR_COLUMN, 2);
        cputs("SPRITE ");
        cputdec(g_state.sprite_number);
        cputs(g_state.sprite_color_mode == SpriteColorModeMono
                ? " MONO    "
                : (g_state.sprite_color_mode == SpriteColorModeMulti ? " MULTI   " : " 16-COL"));
        gotoxy(SIDEBAR_COLUMN, 3);
        textcolor(SchemeAddress);
        cputhex(g_state.sprite_data_addr, 7);
        gotoxy(SIDEBAR_COLUMN, 8);
        textcolor(
            is_sprite_xwidth(g_state.sprite_number) || is_sprite_16color(g_state.sprite_number)
                ? SchemeAccent
                : SchemeUnselected);
        cputs("XWIDE");
        textcolor(is_sprite_hexpand(g_state.sprite_number) ? SchemeAccent : SchemeUnselected);
        cputs(" HEXP");
        textcolor(is_sprite_vexpand(g_state.sprite_number) ? SchemeAccent : SchemeUnselected);
        cputs(" VEXP");
    }
}

static void draw_coordinates(void) {
    if (g_state.redraw_flags & RedrawSidebarCoords) {
        cputncxy(SIDEBAR_COLUMN, SCREEN_ROWS - 1, SIDEBAR_WIDTH, ' ');
        gotoxy(SIDEBAR_COLUMN, SCREEN_ROWS - 1);
        textcolor(SchemeAddress);
        cputc('(');
        cputdec(g_state.cursor_x);
        cputc(',');
        cputdec(g_state.cursor_y);
        cputc(')');
    }
}

static void draw_sprite_preview_area(void) {
    register uint8_t i = 0;
    textcolor(g_state.color[ColorBack]);
    for (i = 0; i < SIDEBAR_PREVIEW_AREA_HEIGHT; ++i) {
        cputncxy(
            SIDEBAR_COLUMN, SIDEBAR_PREVIEW_AREA_TOP + i, SIDEBAR_WIDTH, SOLID_BLOCK_CHARACTER);
    }
}

static void draw_sidebar(void) {
    draw_header();
    draw_side_bar_sprite_info();
    draw_coordinates();
    draw_toolbox();
    draw_color_selector();
    draw_sprite_preview_area();
    g_state.redraw_flags = RedrawNothing;
}

/* Fold the case rather than trust it: line_edit() stores the key as typed. */
static bool answered_yes(const uint8_t* answer) {
    return (answer[0] | 0x20) == 'y' && (answer[1] | 0x20) == 'e' && (answer[2] | 0x20) == 's';
}

/* The status line, cleared.  Three sites asked for it with four one-byte
 * arguments; two of those ride in A and X and the rest are marshalled at every
 * call. */
static void blank_status_row(void) {
    cputncxy(0, SCREEN_ROWS - 1, SCREEN_COLS, ' ');
}

static void ask(const char* question, uint8_t* into, uint8_t max_length) {
    revers(1);
    textcolor(SchemeBar);
    blank_status_row();
    cputsxy(0, SCREEN_ROWS - 1, question);

    /* The prompt stays put and the answer is redrawn after every key, so the
     * caret sits at the end of what has been typed.  line_edit() owns the
     * buffer; the screen and the keyboard stay here.  See
     * test/verify_lineedit.py for the edges. */
    const uint8_t answer_x = (uint8_t)strlen(question);
    uint8_t length = 0;
    into[0] = '\0';
    for (;;) {
        cputncxy(answer_x, SCREEN_ROWS - 1, (uint8_t)(max_length + 1), ' ');
        cputsxy(answer_x, SCREEN_ROWS - 1, (const char*)into);
        cputc(SOLID_BLOCK_CHARACTER);
        if (line_edit((char*)into, (uint8_t)(max_length + 1), &length, cgetc())) {
            break;
        }
    }
    revers(0);
    textcolor(SchemeBackground);
    blank_status_row();
    g_state.redraw_flags |= RedrawSidebarCoords;
}

static void print_key_group(const char* list[], uint8_t count, uint8_t x, uint8_t y) {
    register uint8_t i = 0;
    gotoxy(x, y);
    revers(1);
    textcolor(SchemeBar);
    cputs(list[0]);
    revers(0);
    textcolor(SchemeText);

    for (i = 1; i < count; ++i) {
        gotoxy(x, y + i);
        cputs(list[i]);
    }
}

static void show_help(void) {
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

    textcolor(SchemeCredits);
    revers(1);
    cputncxy(22, SCREEN_ROWS - 4, SCREEN_COLS - 22, 32);
    cputsxy(22, SCREEN_ROWS - 3, " MEGA65 SPRITE EDITOR  V0.10 (C) 2021 HERNAN DI PIETRO    ");
    cputsxy(22, SCREEN_ROWS - 2, " MOUSE/JOY CODE BY PAUL GARDNER-STEPHEN.                  ");
    cputncxy(22, SCREEN_ROWS - 1, SCREEN_COLS - 22, 32);
    revers(0);

    cgetc();
    clrscr();
    VICIV.spr_ena = 7;
}

static void do_exit(void) {
    textcolor(SchemeAccent);
    bordercolor(SchemeAccent);
    bgcolor(SchemeBackground);
    clrscr();
}

static void draw_screen(void) {
    bgcolor(SchemeBackground);
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

static uint16_t joy_delay_countdown = 0;
static unsigned char fire_lock = 0;

/* unsigned short, not uint16_t: uint16_t is unsigned int on this target, and
 * mouse_update_position() takes unsigned short*, a distinct pointer type. */
static unsigned short mx;
static unsigned short my;

static void set_background(void) {
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

static void main_loop(void) {
    static uint8_t edit_color_index = 0;
    static uint8_t colour_countdown = CURSOR_COLOUR_TICKS;
    static uint8_t previous_raster = 0;
    unsigned char buf[64];
    unsigned char key = 0;

    mouse_set_bounding_box(SPRITE_OFFSET_X,
        SPRITE_OFFSET_Y,
        SCREEN_MAX_X + SPRITE_OFFSET_X - POINTER_WIDTH,
        SCREEN_MAX_Y + SPRITE_OFFSET_Y);
    mouse_warp_to(SPRITE_OFFSET_X, POINTER_START_Y);
    mouse_bind_to_sprite(MOUSE_POINTER_NUM);

    while (1) {
        const uint8_t raster = VICIV.rasterline;
        if (raster < previous_raster && --colour_countdown == 0) {
            colour_countdown = CURSOR_COLOUR_TICKS;
            VICIV.spr_color[EDIT_CURSOR_NUM] = SCHEME_CURSOR_RAMP[edit_color_index++ & 0x0F];
        }
        previous_raster = raster;

        mouse_update_position(&mx, &my);
        if ((my >= CANVAS_MOUSE_TOP && my <= CANVAS_MOUSE_BOTTOM) &&
            (mx >= CANVAS_MOUSE_LEFT && mx <= CANVAS_MOUSE_RIGHT)) {
            const uint16_t cell_x = (mx - CANVAS_MOUSE_LEFT) / MOUSE_CELL_PIXELS;
            const uint16_t cell_y = (my - CANVAS_MOUSE_TOP) / MOUSE_CELL_PIXELS;
            if (cell_x != g_state.cursor_x || cell_y != g_state.cursor_y) {
                g_state.draw_cell_fn(g_state.cursor_x, g_state.cursor_y);
                g_state.cursor_x = (uint8_t)cell_x;
                g_state.cursor_y = (uint8_t)cell_y;
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
                            key = KEY_CURSOR_RIGHT;
                            break;
                        case 0xB: // LEFT
                            fire_lock = 0;
                            joy_delay_countdown = JOY_DELAY;
                            key = KEY_CURSOR_LEFT;
                            break;
                        case 0xE: // UP
                            fire_lock = 0;
                            joy_delay_countdown = JOY_DELAY;
                            key = KEY_CURSOR_UP;
                            break;
                        case 0xD: // DOWN
                            fire_lock = 0;
                            joy_delay_countdown = JOY_DELAY;
                            key = KEY_CURSOR_DOWN;
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

            case KEY_HELP:
#ifdef HARNESS_KEYS
            /* HELP does not survive the synthetic keyboard, and F1 is the one
             * function key this editor does not already use. */
            case KEY_F1:
#endif
                show_help();
                erase_canvas_space();
                set_redraw_full_canvas();
                textcolor(SchemeBackground);
                blank_status_row();
                g_state.redraw_flags = RedrawAll;
                break;

                /* ------------------------- CURSOR MOVEMENT GROUP ----------------------------- */

            case KEY_CURSOR_DOWN:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_y =
                    (g_state.cursor_y == g_state.sprite_height - 1) ? 0 : (g_state.cursor_y + 1);
                g_state.redraw_flags = RedrawSidebarCoords | RedrawToolPreview;
                g_state.update_cursor_y_fn();
                break;

            case KEY_CURSOR_UP:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_y =
                    (g_state.cursor_y == 0) ? (g_state.sprite_height - 1) : (g_state.cursor_y - 1);
                g_state.redraw_flags = RedrawSidebarCoords | RedrawToolPreview;
                g_state.update_cursor_y_fn();
                break;

            case KEY_CURSOR_LEFT:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_x =
                    (g_state.cursor_x == 0) ? (g_state.sprite_width - 1) : (g_state.cursor_x - 1);
                g_state.redraw_flags = RedrawSidebarCoords | RedrawToolPreview;
                g_state.update_cursor_x_fn();
                break;

            case KEY_CURSOR_RIGHT:
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

            case KEY_DELETE: {
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
                // VICIV.spr_x64en ^= 1 << PREVIEW_SPRITE_NUM;
                // reg_poke(REG_SPRX64EN, reg_peek(REG_SPRX64EN) ^ (1 <<
                // g_state.sprite_number)); g_state.redraw_flags = RedrawAll;
                // g_state.update_cursor_x_fn = VICIV.spr_x64en & (1 << PREVIEW_SPRITE_NUM) ?
                // update_cursor_xmsb : update_cursor_x; update_and_full_redraw(false);
                // update_sprite_preview();
                break;

            case KEY_RUN_STOP: // also CTRL-C
                ask("COPY SPRITE TO (0-7)? ", buf, 1);
                if (buf[0] >= '0' && buf[0] <= '7') {
                    uint8_t to_sprite = buf[0] - 48;
                    if (sprite_size_bytes(to_sprite) == g_state.sprite_size_bytes) {
                        // Display error if bytes do not match
                        copy_sprite_data(sprite_data_addr(to_sprite));
                    } else {
                        bordercolor(SchemeError);
                        cgetc();
                    }
                    bordercolor(SchemeBorder);
                }
                break;

            case 118: // "V"-expand
                VICIV.spr_exp_y ^= 1 << PREVIEW_SPRITE_NUM;
                reg_poke(REG_SPR_VEXPAND,
                    (uint8_t)(reg_peek(REG_SPR_VEXPAND) ^ (1 << g_state.sprite_number)));
                bordercolor(SchemeBorder);
                update_sprite_preview();
                g_state.redraw_flags = RedrawSidebarInfo;
                break;

            case 104: // "H"-expand
                VICIV.spr_exp_x ^= 1 << PREVIEW_SPRITE_NUM;
                reg_poke(REG_SPR_HEXPAND,
                    (uint8_t)(reg_peek(REG_SPR_HEXPAND) ^ (1 << g_state.sprite_number)));
                bordercolor(SchemeBorder);
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
                cputdec(c_spr_pal_bank);
                reg_poke(REG_SPRPALSEL,
                    (uint8_t)((c_bank_reg & ~0xC) | (((c_spr_pal_bank + 1) % 4) << 2)));
                bordercolor(SchemeBorder);
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

            case KEY_F3:
                ask("EXIT SPRITE EDITOR: ARE YOU SURE (YES/NO)? ", buf, 3);
                if (answered_yes(buf)) {
                    return;
                }
                break;

            case KEY_F5: // load
            case KEY_F7: // save
                // TODO: implement
                break;

            case KEY_F9: // fetch from slot
                update_and_full_redraw(true);
                break;

            case KEY_F11: // save to slot
                put_sprite_data_to_slot();
                bordercolor(SchemeBorder);
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

void do_sprite_editor(void) {
    initialize();
    draw_screen();
    main_loop();
    do_exit();
}
