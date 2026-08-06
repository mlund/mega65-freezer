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
#define PAGE_SIZE 256
#define LOCAL_VIC_BASE 0xD000
#ifdef SPRITED_STANDALONE
#define VIC_BASE LOCAL_VIC_BASE
#define CIA2_PORT_A 0xDD00UL
#define FREEZE_PEEK(x) PEEK((x))
#define FREEZE_POKE(x, y) POKE((x), (y))
#else
#define VIC_BASE 0xFFD3000UL // This is where VIC-II is mapped in frozen memory
#define CIA2_PORT_A 0xFFD3D00UL
#define FREEZE_PEEK(x) freeze_peek((x))
#define FREEZE_POKE(x, y) freeze_poke((x), (y))
#endif
#define REG_SPRPTR_B0 (FREEZE_PEEK(VIC_BASE + 0x6CUL))
#define REG_SPRPTR_B1 (FREEZE_PEEK(VIC_BASE + 0x6DUL))
#define REG_SPRPTR_B2 (FREEZE_PEEK(VIC_BASE + 0x6EUL) & 0x7F)
#define REG_SPRPTR16 (FREEZE_PEEK(VIC_BASE + 0x6EUL) & 0x80)
#define REG_SPR_VEXPAND (VIC_BASE + 0x17UL)
#define REG_SPR_HEXPAND (VIC_BASE + 0x1DUL)
#define REG_SPR_16COL (VIC_BASE + 0x6BUL)
#define REG_SPR_MULTICOLOR (VIC_BASE + 0x1CUL)
#define REG_SPRX64EN (VIC_BASE + 0x57UL)
#define REG_SPRITE_MULTICOL1 (VIC_BASE + 0x25UL)
#define REG_SPRITE_MULTICOL2 (VIC_BASE + 0x26UL)
#define REG_SPRITE_COLOR(n) (VIC_BASE + 0x27UL + (n))
#define REG_SPRPALSEL (VIC_BASE + 0x70UL)

#define LOCAL_REG_SPR_16COL (LOCAL_VIC_BASE + 0x6BUL)
#define LOCAL_REG_SPR_MULTICOLOR (LOCAL_VIC_BASE + 0x1CUL)
#define LOCAL_REG_SPRX64EN (LOCAL_VIC_BASE + 0x57UL)
#define LOCAL_REG_SPRITE_MULTICOL1 (LOCAL_VIC_BASE + 0x25UL)
#define LOCAL_REG_SPRITE_MULTICOL2 (LOCAL_VIC_BASE + 0x26UL)
#define LOCAL_REG_SPRITE_COLOR(n) (LOCAL_VIC_BASE + 0x27UL + (n))
#define LOCAL_REG_SPRPALSEL (LOCAL_VIC_BASE + 0x70UL)

#define SPRITE_PALETTE ((FREEZE_PEEK(REG_SPRPALSEL) & 0x30) >> 4)
#define IS_SPR_MULTICOLOR(n) ((FREEZE_PEEK(REG_SPR_MULTICOLOR)) & (1 << (n)))
#define IS_SPR_16COL(n) ((FREEZE_PEEK(REG_SPR_16COL)) & (1 << (n)))
#define IS_SPR_XWIDTH(n) ((FREEZE_PEEK(REG_SPRX64EN)) & (1 << (n)))
#define IS_SPR_HEXPAND(n) ((FREEZE_PEEK(REG_SPR_HEXPAND)) & (1 << (n)))
#define IS_SPR_VEXPAND(n) ((FREEZE_PEEK(REG_SPR_VEXPAND)) & (1 << (n)))
#define SPRITE_POINTER_ADDR                                                                        \
    (((long)REG_SPRPTR_B0) | ((long)REG_SPRPTR_B1 << 8) | ((long)REG_SPRPTR_B2 << 16))
#define SPRITE_SIZE_BYTES(n) ((IS_SPR_XWIDTH((n)) | IS_SPR_16COL((n))) ? 168 : 64)
#define SPRITE_DATA_ADDR(n)                                                                        \
    (REG_SPRPTR16 ? 64 *                                                                           \
                (((long)FREEZE_PEEK(SPRITE_POINTER_ADDR + 1 + (n) * 2) << 8) +                     \
                    ((long)FREEZE_PEEK(SPRITE_POINTER_ADDR + (n) * 2)))                            \
                  : (long)(64 * FREEZE_PEEK(SPRITE_POINTER_ADDR + (n))) |                          \
                (((long)(~FREEZE_PEEK(CIA2_PORT_A) & 0x3)) << 14))
// #define REG_SPRBPMEN_0_3            (vic_registers[0x49] >> 4)
// #define REG_SPRBPMEN_4_7            (vic_registers[0x4B] >> 4)
// #define SPRITE_BITPLANE_ENABLE(n)	(((REG_SPRBPMEN_4_7) << 4 | REG_SPRBPMEN_0_3) & (1 << (n)))
#define SCREEN_ROWS 25
#define SCREEN_COLS 80
#define CANVAS_HEIGHT (SCREEN_ROWS - 2)
#define CANVAS_TOP_MARGIN 2

#define SPRITE_MAX_COUNT 8
#define DEFAULT_BORDER_COLOR 6
#define DEFAULT_SCREEN_COLOR 6
#define DEFAULT_BACK_COLOR 11

#define TRANS_CHARACTER 230
#define SOLID_BLOCK_CHARACTER 224
#define SHAPE_PREVIEW_CHARACTER 32
#define SIDEBAR_COLUMN 65
#define SIDEBAR_WIDTH (SCREEN_COLS - SIDEBAR_COLUMN)
#define SIDEBAR_PREVIEW_AREA_TOP 10
#define SIDEBAR_PREVIEW_AREA_BOTTOM 20
#define SIDEBAR_PREVIEW_AREA_HEIGHT (SIDEBAR_PREVIEW_AREA_BOTTOM - SIDEBAR_PREVIEW_AREA_TOP)
#define SPRITE_OFFSET_X 24
#define SPRITE_OFFSET_Y 50

#define JOY_DELAY 10000U

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS8(x) (((x) ^ ((x) >> 7)) - ((x) >> 7))
#define ABS16(x) (((x) ^ ((x) >> 15)) - ((x) >> 15))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

// Screen RAM for our area. We do not use 16-bit character mode
// so we need 80x25 = 2K area.
#define SCREEN_RAM_ADDRESS 0x12000UL
#define CHARSET_ADDRESS 0x15000UL
#define SPRITE_POINTER_TABLE 0x16000UL
#define SPRITE_BUFFER 0x40000UL
#define PREVIEW_SPRITE_NUM 2
#define EDIT_CURSOR_NUM 1

// Redraw flags
#define REDRAW_SB_NONE 0
#define REDRAW_SB_INFO 1
#define REDRAW_SB_COORD 2
#define REDRAW_SB_COLOR 4
#define REDRAW_SB_TOOLS 8
#define REDRAW_TOOL_PREVIEW 16
#define REDRAW_TOOL_HEADER 32
#define REDRAW_SB_ALL 0x3f

/* mega65-libc types screen text as uint8_t*, while C string literals are
 * char[].  These convert once so call sites stay free of casts. */
static inline void screen_puts(const char* s) {
    cputs((const uint8_t*)s);
}
static inline void screen_putsxy(uint8_t x, uint8_t y, const char* s) {
    cputsxy(x, y, (const uint8_t*)s);
}

#define SPR_COLOR_MODE_MONOCHROME 0
#define SPR_COLOR_MODE_MULTICOLOR 1
#define SPR_COLOR_MODE_16COLOR 2

// Drawing tools
#define DRAWING_TOOL_PIXEL 0
#define DRAWING_TOOL_LINE 1
#define DRAWING_TOOL_BOX 2
#define DRAWING_TOOL_FILLEDBOX 3
#define DRAWING_TOOL_CIRCLE 4
#define DRAWING_TOOL_FILLED_CIRCLE 5

// Index into color array
#define COLOR_BACK 0
#define COLOR_FORE 1
#define COLOR_MC1 2
#define COLOR_MC2 3

typedef void (*PaintFunc)(uint8_t, uint8_t);

typedef struct TagAppstate {
    uint8_t wide_screen_mode;
    uint8_t sprite_number;
    uint8_t sprite_color_mode;
    uint8_t sprite_width, sprite_height;
    uint8_t cells_per_pixel, bytes_per_row, pixels_per_byte;
    uint8_t color[4];
    uint8_t color_source[4];
    uint8_t current_color_idx;
    uint8_t cursor_x, cursor_y;
    uint8_t canvas_left_x;
    uint8_t drawing_tool;
    uint8_t tool_active, tool_org_x, tool_org_y, fill_shape;
    uint8_t redraw_flags; // See REDRAW_SB_ constants for flags
    RECT redraw_rect;
    void (*draw_cell_fn)(uint8_t, uint8_t);
    void (*paint_cell_fn)(uint8_t, uint8_t);
    void (*draw_shape_fn)(PaintFunc);
    void (*update_cursor_x_fn)(void);
    void (*update_cursor_y_fn)(void);
    unsigned int sprite_size_bytes;
    long sprite_data_addr;
} APPSTATE;

typedef struct TagFileoptions {
    uint8_t mode;
    uint8_t name[16];
} FILEOPTIONS;

static APPSTATE g_state;

/* Nonstatic Function prototypes */
void update_sprite_parameters(bool);
void set_draw_tool(uint8_t);
void set_redraw_full_canvas(void);
void set_effective_tool_rect(RECT*);
void setup_text_palette(void);
void update_cursor_x(void);
void update_cursor_y(void);
void update_cursor_xmsb(void);

/* Toolbox Character set, in order of DRAWING_TOOL... enumeration */

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

    g_state.redraw_flags = REDRAW_SB_ALL;
    g_state.sprite_number = 0;
    g_state.cursor_x = g_state.cursor_y = 0;
    g_state.current_color_idx = COLOR_FORE;
    g_state.tool_active = 0;
    g_state.tool_org_x = g_state.tool_org_y = 0;
    g_state.color[COLOR_BACK] = DEFAULT_BACK_COLOR;
    g_state.wide_screen_mode = 0;
    g_state.update_cursor_x_fn = update_cursor_x;
    g_state.update_cursor_y_fn = update_cursor_y;

    set_draw_tool(DRAWING_TOOL_PIXEL);
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
    // We do this because ALTPAL is selected by VIC-III HIGHLIGHT+UNDERLINE
    // combination and  Foreground colors in alt-palette are used from the 16th index,
    // so we avoid fiddling with this.

    // POKE(0xD070UL, (PEEK(0xD070UL) & ~48) | ((FREEZE_PEEK(REG_SPRPALSEL) & 0xC) << 2));
    // setmapedpal(SPRITE_PALETTE);
}

void update_cursor_x() {
    uint8_t cvw = g_state.canvas_left_x * 4;
    uint8_t xc = g_state.cursor_x * g_state.cells_per_pixel * 4;
    POKE(0xD002, SPRITE_OFFSET_X + cvw + xc);
}

void update_cursor_y() {
    uint8_t yc = g_state.cursor_y * 8;
    POKE(0xD003, SPRITE_OFFSET_Y + (2 * 8) + yc);
}

void update_cursor_xmsb() {
    uint8_t cvw = g_state.canvas_left_x * 4;
    uint8_t xc = g_state.cursor_x * g_state.cells_per_pixel * 4;
    const unsigned short sx = SPRITE_OFFSET_X + cvw + xc;
    if (sx < 256) {
        POKE(0xD010, PEEK(0xD010) & ~(1 << EDIT_CURSOR_NUM));
    } else {
        POKE(0xD010, PEEK(0xD010) | (1 << EDIT_CURSOR_NUM));
    }
    POKE(0xD002, sx);
}

static void draw_shape_char(uint8_t x, uint8_t y) {
    cputncxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel),
        2 + y,
        g_state.cells_per_pixel,
        SHAPE_PREVIEW_CHARACTER);
}

static void draw_line(PaintFunc pfun) {
    RECT rc;
    set_effective_tool_rect(&rc);
    if (g_state.tool_org_y == g_state.cursor_y) // Horizontal
    {
        register uint8_t x = rc.left;
        while (x <= rc.right) {
            pfun(x++, g_state.cursor_y);
        }
    } else if (g_state.tool_org_x == g_state.cursor_x) // Vertical
    {
        register uint8_t y = rc.top;
        while (y <= rc.bottom) {
            pfun(g_state.cursor_x, y++);
        }
    } else // Bresenham- algorithm.
    {
        const signed char dx = rc.right - rc.left;
        const signed char dy = -(rc.bottom - rc.top);
        uint8_t x = g_state.tool_org_x, y = g_state.tool_org_y;
        signed char e = dx + dy;
        signed char e2 = 0;
        signed char sx = g_state.cursor_x > g_state.tool_org_x ? 1 : -1;
        signed char sy = g_state.cursor_y > g_state.tool_org_y ? 1 : -1;

        for (;;) {
            pfun(x, y);
            if (x == g_state.cursor_x && y == g_state.cursor_y) {
                break;
            }
            e2 = e * 2;
            if (e2 >= dy) {
                e += dy;
                x += sx;
            }
            if (e2 <= dx) {
                e += dx;
                y += sy;
            }
        }
    }
    clearattr();
}

/*
static void DrawCircle(PaintFunc pfun)
{
  RECT rc;
  SetEffectiveToolRect(&rc);

  // RECT rc;
  // void (*pfun)(uint8_t, uint8_t) = bPreview ? DrawShapeChar : g_state.paintCellFn;
  // SetEffectiveToolRect(&rc);

  // const signed char dx = rc.right - rc.left;
  // const signed char dy = -(rc.bottom - rc.top);
  // uint8_t x = g_state.toolOrgX, y = g_state.toolOrgY;
  // signed char e = dx + dy;
  // signed char e2 = 0;
  // signed char sx = g_state.cursorX > g_state.toolOrgX ? 1 : -1;
  // signed char sy = g_state.cursorY > g_state.toolOrgY ? 1 : -1;

  // for (;;)
  // {
  //     pfun(x, y);
  //     if (x == g_state.cursorX && y == g_state.cursorY)
  //         break;
  //     e2 = e * 2;
  //     if (e2 >= dy)
  //     {
  //         e += dy;
  //         x += sx;
  //     }
  //     if (e2 <= dx)
  //     {
  //         e += dx;
  //         y += sy;
  //     }
  // }
}
*/

static void draw_box(PaintFunc pfun) {
    RECT rc;
    register uint8_t x, y, i;
    set_effective_tool_rect(&rc);
    x = rc.left;
    while (x <= rc.right) {
        pfun(x, g_state.cursor_y);
        if (g_state.fill_shape) {
            for (i = rc.top + 1; i < rc.bottom; ++i) {
                pfun(x, i);
            }
        }
        pfun(x++, g_state.tool_org_y);
    }

    y = rc.top;
    while (y <= rc.bottom) {
        pfun(g_state.cursor_x, y);
        pfun(g_state.tool_org_x, y++);
    }
    clearattr();
}

static void draw_nothing(PaintFunc pfun) {
    (void)pfun;
}

void set_draw_tool(uint8_t dt) {
    g_state.drawing_tool = dt;
    switch (dt) {
        case DRAWING_TOOL_BOX:
            g_state.fill_shape = 0;
            g_state.draw_shape_fn = draw_box;
            break;
        case DRAWING_TOOL_FILLEDBOX:
            g_state.fill_shape = 1;
            g_state.draw_shape_fn = draw_box;
            break;
        case DRAWING_TOOL_LINE:
            g_state.draw_shape_fn = draw_line;
            break;
        case DRAWING_TOOL_PIXEL:
        default:
            g_state.draw_shape_fn = draw_nothing;
    }
}

static void draw_mono_cell(uint8_t x, uint8_t y) {
    register uint8_t cell = 0;
    const uint8_t bufoff = y * g_state.sprite_width / 8;
    const long byte_addr = (SPRITE_BUFFER + bufoff) + (x / 8);
    const uint8_t p = lpeek(byte_addr) & (0x80 >> (x % 8));

    gotoxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel), y + 2);
    for (cell = 0; cell < g_state.cells_per_pixel; ++cell) {
        textcolor(p ? g_state.color[COLOR_FORE] : g_state.color[COLOR_BACK]);
        cputc(p ? SOLID_BLOCK_CHARACTER : TRANS_CHARACTER);
    }
}

static void draw16_color_cell(uint8_t x, uint8_t y) {
    register uint8_t cell = 0;
    const long byte_addr = (SPRITE_BUFFER + (y * 8)) + (x / 2);
    const uint8_t p = 0xF & (lpeek(byte_addr) >> (((x + 1) % 2) * 4));
    // const uint8_t col = (g_state.spriteNumber * 16) + p;

    gotoxy(g_state.canvas_left_x + (x * g_state.cells_per_pixel), y + 2);
    for (cell = 0; cell < g_state.cells_per_pixel; ++cell) {
        textcolor(p);
        cputc(p ? SOLID_BLOCK_CHARACTER : TRANS_CHARACTER);
    }
}

static void draw_multicolor_cell(uint8_t x, uint8_t y) {
    register uint8_t cell = 0;
    const long byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 4)) + (x / 4);
    const uint8_t b = lpeek(byte_addr);
    const uint8_t p0 = b & (0x80 >> (2 * (x % 4)));
    const uint8_t p1 = b & (0x40 >> (2 * (x % 4)));
    uint8_t color = g_state.color[COLOR_BACK];
    if (!p0 && p1) {
        color = g_state.color[COLOR_MC1];
    } else if (p0 && !p1) {
        color = g_state.color[COLOR_FORE];
    } else if (p0 && p1) {
        color = g_state.color[COLOR_MC2];
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
    lpoke(byte_addr, g_state.current_color_idx == COLOR_BACK ? (b & ~bitsel) : (b | bitsel));
}

static void paint_pixel_multi(uint8_t x, uint8_t y) {
    const long byte_addr = (SPRITE_BUFFER + (y * g_state.sprite_width / 4)) + (x / 4);
    const uint8_t bitsel = (2 * (x % 4));
    const uint8_t mask = ((0x80 >> bitsel) | (0x40 >> bitsel));
    if (g_state.current_color_idx == COLOR_BACK) {
        lpoke(byte_addr, lpeek(byte_addr) & ~mask);
    } else {
        if (g_state.current_color_idx == COLOR_FORE) {
            lpoke(byte_addr, lpeek(byte_addr) & ~mask | (0x80 >> bitsel));
        } else if (g_state.current_color_idx == COLOR_MC1) {
            lpoke(byte_addr, lpeek(byte_addr) & ~mask | (0x40 >> bitsel));
        } else if (g_state.current_color_idx == COLOR_MC2) {
            lpoke(byte_addr, lpeek(byte_addr) & ~mask | ((0x80 >> bitsel) | (0x40 >> bitsel)));
        }
    }
}

static void paint_pixel16_color(uint8_t x, uint8_t y) {
    const long byte_addr = (SPRITE_BUFFER + (y * 8)) + (x / 2);
    const uint8_t bitsel = (((x + 1) % 2) * 4);
    lpoke(byte_addr,
        lpeek(byte_addr) & (0xF0 >> bitsel) | (g_state.color[g_state.current_color_idx] << bitsel));
}

static void clear_sprite() {
    set_redraw_full_canvas();
    lfill(SPRITE_BUFFER, 0, g_state.sprite_size_bytes);
}

static void fetch_vic2_regs_from_slot() {
    // H/Y expand

    const uint8_t spr_bit = 1 << PREVIEW_SPRITE_NUM;
    if (IS_SPR_HEXPAND(g_state.sprite_number)) {
        POKE(0xD01D, PEEK(0xD01D) | spr_bit);
    } else {
        POKE(0xD01D, PEEK(0xD01D) & ~spr_bit);
    }
    if (IS_SPR_VEXPAND(g_state.sprite_number)) {
        POKE(0xD017, PEEK(0xD017) | spr_bit);
    } else {
        POKE(0xD017, PEEK(0xD017) & ~spr_bit);
    }
    if (IS_SPR_XWIDTH(g_state.sprite_number)) {
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

static void update_palette(void) {
    // register uint8_t i = 0;
    // if (IS_SPR_16COL(g_state.spriteNumber)) {

    //     setmapedpal( (FREEZE_PEEK(REG_SPRPALSEL) >> 2) & 0x3);
    //     for (i = 0; i < 16; ++i) {
    //         POKE(0xD100 + i,
    //     }
    // }
}

#define HFACTOR (IS_SPR_HEXPAND(g_state.sprite_number) ? 2 : 1)
#define VFACTOR (IS_SPR_VEXPAND(g_state.sprite_number) ? 2 : 1)

static void update_sprite_preview(void) {
    // Setup Preview Area sprite. (we divide by 2 for H320 sprites, should divide by 1 if H640 mode)

    POKE(LOCAL_REG_SPRITE_COLOR(PREVIEW_SPRITE_NUM), g_state.color[COLOR_FORE]);
    POKE(LOCAL_REG_SPRITE_MULTICOL1, g_state.color[COLOR_MC1]);
    POKE(LOCAL_REG_SPRITE_MULTICOL2, g_state.color[COLOR_MC2]);

    POKE(0xD004,
        (SPRITE_OFFSET_X +
            ((SIDEBAR_COLUMN * 8 / 2) +
                (((SIDEBAR_WIDTH * 8 / 2) / 2) -
                    (g_state.sprite_width * HFACTOR /
                        (IS_SPR_MULTICOLOR(g_state.sprite_number) ? 1 : 2))))) &
            0xFF);

    POKE(0xD005,
        (SPRITE_OFFSET_Y + (SIDEBAR_PREVIEW_AREA_TOP * 8) +
            (((SIDEBAR_PREVIEW_AREA_HEIGHT * 8) / 2) - (g_state.sprite_height * VFACTOR / 2))));
}

void update_sprite_parameters(bool f_fetch_slot) {
    const uint8_t is_x_width = IS_SPR_XWIDTH(g_state.sprite_number);

    g_state.sprite_height = 21;
    g_state.sprite_size_bytes = SPRITE_SIZE_BYTES(g_state.sprite_number);
    g_state.bytes_per_row = g_state.sprite_size_bytes / g_state.sprite_height;
    g_state.sprite_data_addr = SPRITE_DATA_ADDR(g_state.sprite_number);

    if (f_fetch_slot) {
        fetch_sprite_data_from_slot();
        fetch_vic2_regs_from_slot();
    }

    if (IS_SPR_16COL(g_state.sprite_number)) {
        g_state.draw_cell_fn = draw16_color_cell;
        g_state.paint_cell_fn = paint_pixel16_color;
        g_state.sprite_color_mode = SPR_COLOR_MODE_16COLOR;
        g_state.sprite_width = 16; // Extended width is implied for 16-color sprites.
        g_state.cells_per_pixel = 3;
        g_state.pixels_per_byte = 2;
        g_state.current_color_idx = COLOR_FORE;
        POKE(LOCAL_REG_SPR_16COL, PEEK(LOCAL_REG_SPR_16COL) | (1 << PREVIEW_SPRITE_NUM));
        POKE(LOCAL_REG_SPR_MULTICOLOR, PEEK(LOCAL_REG_SPR_MULTICOLOR) & ~(1 << PREVIEW_SPRITE_NUM));
    } else if (IS_SPR_MULTICOLOR(g_state.sprite_number)) {
        g_state.draw_cell_fn = draw_multicolor_cell;
        g_state.paint_cell_fn = paint_pixel_multi;
        g_state.sprite_color_mode = SPR_COLOR_MODE_MULTICOLOR;
        g_state.sprite_width = is_x_width ? 32 : 12;
        g_state.cells_per_pixel = is_x_width ? 2 : 4;
        g_state.pixels_per_byte = 4;
        g_state.color[COLOR_FORE] = FREEZE_PEEK(REG_SPRITE_COLOR(g_state.sprite_number));
        g_state.color[COLOR_MC1] = FREEZE_PEEK(REG_SPRITE_MULTICOL1);
        g_state.color[COLOR_MC2] = FREEZE_PEEK(REG_SPRITE_MULTICOL2);
        POKE(LOCAL_REG_SPR_16COL, PEEK(LOCAL_REG_SPR_16COL) & ~(1 << PREVIEW_SPRITE_NUM));
        POKE(LOCAL_REG_SPR_MULTICOLOR, PEEK(LOCAL_REG_SPR_MULTICOLOR) | (1 << PREVIEW_SPRITE_NUM));
    } else {
        g_state.draw_cell_fn = draw_mono_cell;
        g_state.paint_cell_fn = paint_pixel_mono;
        g_state.sprite_color_mode = SPR_COLOR_MODE_MONOCHROME;
        g_state.sprite_width = is_x_width ? 64 : 24;
        g_state.cells_per_pixel = is_x_width ? 1 : 2;
        g_state.pixels_per_byte = 8;
        g_state.color[COLOR_FORE] = FREEZE_PEEK(REG_SPRITE_COLOR(g_state.sprite_number));
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
    FREEZE_POKE(REG_SPRITE_COLOR(g_state.sprite_number), g_state.color[COLOR_FORE]);
    FREEZE_POKE(REG_SPRITE_MULTICOL1, g_state.color[COLOR_MC1]);
    FREEZE_POKE(REG_SPRITE_MULTICOL2, g_state.color[COLOR_MC2]);
    bordercolor(COLOUR_BLUE);

    POKE(LOCAL_REG_SPRITE_COLOR(PREVIEW_SPRITE_NUM), g_state.color[COLOR_FORE]);
    POKE(LOCAL_REG_SPRITE_MULTICOL1, g_state.color[COLOR_MC1]);
    POKE(LOCAL_REG_SPRITE_MULTICOL2, g_state.color[COLOR_MC2]);
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

    if (g_state.tool_active && (g_state.redraw_flags & REDRAW_TOOL_PREVIEW)) {
        blink(1);
        revers(1);
        textcolor(g_state.color[g_state.current_color_idx]);
        g_state.draw_shape_fn(draw_shape_char);
    }

    set_rect(&g_state.redraw_rect, 0, 0, 0, 0);
    g_state.redraw_flags &= ~REDRAW_TOOL_PREVIEW;
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

static void draw_header() {
    if (g_state.redraw_flags & REDRAW_TOOL_HEADER) {
        /* Escape names stay lowercase -- libc matches them by hash -- but the
         * text must be uppercase, because cprintf runs petsciitoscreencode()
         * over it. */
        cprintf((const uint8_t*)"{home}{rvson}{lgrn}                            THE MEGA65 SPRITE "
                                "EDITOR           "
                                "                 {rvsoff}");
    }
}

static void draw_color_selector() {
    RECT rc;
    if (g_state.redraw_flags & REDRAW_SB_COLOR) {
        set_rect(&rc, SIDEBAR_COLUMN, 5, 80, 7);
        fillrect(&rc, ' ', DEFAULT_SCREEN_COLOR);

        switch (g_state.sprite_color_mode) {
            case SPR_COLOR_MODE_MONOCHROME:

                textcolor(g_state.color[COLOR_BACK]);
                screen_putsxy(SIDEBAR_COLUMN, 5, "\xe0\xe0\xe0\xe0\xe0\xe0");
                textcolor(g_state.color[COLOR_FORE]);
                screen_putsxy(SIDEBAR_COLUMN + 8, 5, "\xe0\xe0\xe0\xe0\xe0\xe0");

                textcolor(g_state.current_color_idx == COLOR_BACK ? 1 : COLOUR_DARKGREY);
                screen_putsxy(SIDEBAR_COLUMN + 2, 6, "BK");

                textcolor(g_state.current_color_idx == COLOR_FORE ? 1 : COLOUR_DARKGREY);
                screen_putsxy(SIDEBAR_COLUMN + 8 + 2, 6, "FG");

                break;

            case SPR_COLOR_MODE_16COLOR:
                textcolor(g_state.color[COLOR_FORE]);
                screen_putsxy(SIDEBAR_COLUMN,
                    5,
                    "\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0\xe0");

                textcolor(1);
                screen_putsxy(SIDEBAR_COLUMN + 2,
                    6,
                    g_state.color[COLOR_FORE] == 0 ? "BACKGROUND" : "FOREGROUND");
                break;

            case SPR_COLOR_MODE_MULTICOLOR:
                textcolor(g_state.color[COLOR_BACK]);
                screen_putsxy(SIDEBAR_COLUMN, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[COLOR_FORE]);
                screen_putsxy(SIDEBAR_COLUMN + 4, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[COLOR_MC1]);
                screen_putsxy(SIDEBAR_COLUMN + 4 * 2, 5, "\xe0\xe0\xe0");
                textcolor(g_state.color[COLOR_MC2]);
                screen_putsxy(SIDEBAR_COLUMN + 4 * 3, 5, "\xe0\xe0\xe0");

                textcolor(COLOUR_DARKGREY);
                screen_putsxy(SIDEBAR_COLUMN + 1, 6, "BK");
                screen_putsxy(SIDEBAR_COLUMN + 5, 6, "FG");
                screen_putsxy(SIDEBAR_COLUMN + 8, 6, "MC1");
                screen_putsxy(SIDEBAR_COLUMN + 12, 6, "MC2");

                textcolor(1);
                switch (g_state.current_color_idx) {
                    case COLOR_BACK:
                        screen_putsxy(SIDEBAR_COLUMN + 1, 6, "BK");
                        break;
                    case COLOR_FORE:
                        screen_putsxy(SIDEBAR_COLUMN + 5, 6, "FG");
                        break;
                    case COLOR_MC1:
                        screen_putsxy(SIDEBAR_COLUMN + 8, 6, "MC1");
                        break;
                    case COLOR_MC2:
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

    if (g_state.redraw_flags & REDRAW_SB_TOOLS) {
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
    if (g_state.redraw_flags & REDRAW_SB_INFO) {
        textcolor(1);
        gotoxy(SIDEBAR_COLUMN, 2);
        screen_puts("SPRITE ");
        cputdec(g_state.sprite_number, 0, 0);
        screen_puts(g_state.sprite_color_mode == SPR_COLOR_MODE_MONOCHROME
                ? " MONO    "
                : (g_state.sprite_color_mode == SPR_COLOR_MODE_MULTICOLOR ? " MULTI   "
                                                                          : " 16-COL"));
        gotoxy(SIDEBAR_COLUMN, 3);
        textcolor(3);
        cputhex(g_state.sprite_data_addr, 7);
        gotoxy(SIDEBAR_COLUMN, 8);
        textcolor(IS_SPR_XWIDTH(g_state.sprite_number) | IS_SPR_16COL(g_state.sprite_number)
                ? COLOUR_LIGHTBLUE
                : COLOUR_DARKGREY);
        screen_puts("XWIDE");
        textcolor(IS_SPR_HEXPAND(g_state.sprite_number) ? COLOUR_LIGHTBLUE : COLOUR_DARKGREY);
        screen_puts(" HEXP");
        textcolor(IS_SPR_VEXPAND(g_state.sprite_number) ? COLOUR_LIGHTBLUE : COLOUR_DARKGREY);
        screen_puts(" VEXP");
    }
}

static void draw_coordinates() {
    if (g_state.redraw_flags & REDRAW_SB_COORD) {
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
    textcolor(g_state.color[COLOR_BACK]);
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
    g_state.redraw_flags = REDRAW_SB_NONE;
}

static void ask(const char* question, uint8_t* outbuffer, uint8_t maxlen) {
    gotoy(SCREEN_ROWS - 1);
    revers(1);
    textcolor(COLOUR_PINK);
    cputncxy(0, SCREEN_ROWS - 1, SCREEN_COLS, ' ');
    screen_putsxy(0, SCREEN_ROWS - 1, question);
    cinput(outbuffer, maxlen + 1, CINPUT_ACCEPT_ALL);
    revers(0);
    textcolor(COLOUR_BLUE);
    cputncxy(0, SCREEN_ROWS - 1, SCREEN_COLS, ' ');
    g_state.redraw_flags |= REDRAW_SB_COORD;
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
    g_state.redraw_flags |= REDRAW_TOOL_HEADER;
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
    g_state.redraw_flags = REDRAW_SB_COLOR;
    if (g_state.sprite_color_mode == SPR_COLOR_MODE_16COLOR) {
        g_state.color[g_state.current_color_idx] = 0;
    } else {
        g_state.current_color_idx = COLOR_BACK;
    }
}

static void main_loop() {
    static uint8_t edit_color_counter = 0;
    unsigned char buf[64];
    unsigned char key = 0, keymod = 0;

    mouse_set_bounding_box(0 + 24, 0 + 50, 319 + 24 - 8, 199 + 50);
    mouse_warp_to(24, 100);
    mouse_bind_to_sprite(0);

    while (1) {
        POKE(
            0xD028, EDIT_CURSOR_COLOR_MAP[edit_color_counter++ / 16]); // Update editor cursor color

        mouse_update_position(&mx, &my);
        if ((my >= 66 && my <= 233) && (mx >= 55 && mx <= 235)) {
            if ((((mx - 55) / 8) != g_state.cursor_x) || (((my - 66) / 8) != g_state.cursor_y)) {
                g_state.draw_cell_fn(g_state.cursor_x, g_state.cursor_y);
                g_state.cursor_x = (mx - 55) / 8;
                g_state.cursor_y = (my - 66) / 8;
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
                g_state.redraw_flags = REDRAW_SB_ALL;
                break;

                /* ------------------------- CURSOR MOVEMENT GROUP ----------------------------- */

            case CH_CURS_DOWN:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_y =
                    (g_state.cursor_y == g_state.sprite_height - 1) ? 0 : (g_state.cursor_y + 1);
                g_state.redraw_flags = REDRAW_SB_COORD | REDRAW_TOOL_PREVIEW;
                g_state.update_cursor_y_fn();
                break;

            case CH_CURS_UP:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_y =
                    (g_state.cursor_y == 0) ? (g_state.sprite_height - 1) : (g_state.cursor_y - 1);
                g_state.redraw_flags = REDRAW_SB_COORD | REDRAW_TOOL_PREVIEW;
                g_state.update_cursor_y_fn();
                break;

            case CH_CURS_LEFT:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_x =
                    (g_state.cursor_x == 0) ? (g_state.sprite_width - 1) : (g_state.cursor_x - 1);
                g_state.redraw_flags = REDRAW_SB_COORD | REDRAW_TOOL_PREVIEW;
                g_state.update_cursor_x_fn();
                break;

            case CH_CURS_RIGHT:
                g_state.draw_shape_fn(g_state.draw_cell_fn);
                g_state.cursor_x =
                    (g_state.cursor_x == g_state.sprite_width - 1) ? 0 : (g_state.cursor_x + 1);
                g_state.redraw_flags = REDRAW_SB_COORD | REDRAW_TOOL_PREVIEW;
                g_state.update_cursor_x_fn();
                break;

                /* ------------------------- EDIT GROUP ------------------------------------------
                 */

            case ' ':

                if (g_state.drawing_tool == DRAWING_TOOL_PIXEL) {
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
                const uint8_t c_index = g_state.current_color_idx;
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
                g_state.redraw_flags = REDRAW_SB_ALL;

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
                g_state.redraw_flags = REDRAW_SB_ALL;
                switch (g_state.sprite_color_mode) {
                    case SPR_COLOR_MODE_16COLOR:
                        // Switch to Hi-Res
                        FREEZE_POKE(REG_SPR_16COL,
                            FREEZE_PEEK(REG_SPR_16COL) & ~(1 << g_state.sprite_number));
                        FREEZE_POKE(REG_SPR_MULTICOLOR,
                            FREEZE_PEEK(REG_SPR_MULTICOLOR) & ~(1 << g_state.sprite_number));
                        break;
                    case SPR_COLOR_MODE_MULTICOLOR:
                        // Switch to 16-col
                        FREEZE_POKE(REG_SPR_16COL,
                            FREEZE_PEEK(REG_SPR_16COL) | (1 << g_state.sprite_number));
                        FREEZE_POKE(REG_SPR_MULTICOLOR,
                            FREEZE_PEEK(REG_SPR_MULTICOLOR) & ~(1 << g_state.sprite_number));
                        break;
                    case SPR_COLOR_MODE_MONOCHROME:
                        // Switch to Multicol
                        FREEZE_POKE(REG_SPR_16COL,
                            FREEZE_PEEK(REG_SPR_16COL) & ~(1 << g_state.sprite_number));
                        FREEZE_POKE(REG_SPR_MULTICOLOR,
                            FREEZE_PEEK(REG_SPR_MULTICOLOR) | (1 << g_state.sprite_number));
                        break;
                    default:
                        break;
                }
                update_and_full_redraw(false);
                break;

            case '@': // 94: // Up-arrow
                // TODO: Disabled until we can fix it; issue #77
                // POKE(LOCAL_REG_SPRX64EN, PEEK(LOCAL_REG_SPRX64EN) ^ (1 << PREVIEW_SPRITE_NUM));
                // FREEZE_POKE(REG_SPRX64EN, FREEZE_PEEK(REG_SPRX64EN) ^ (1 <<
                // g_state.spriteNumber)); g_state.redrawFlags = REDRAW_SB_ALL;
                // g_state.updateCursorXFn = PEEK(LOCAL_REG_SPRX64EN) & (1 << PREVIEW_SPRITE_NUM) ?
                // UpdateCursorXMSB : UpdateCursorX; UpdateAndFullRedraw(false);
                // UpdateSpritePreview();
                break;

            case 3: // CTRL-C
                ask("COPY SPRITE TO (0-7)? ", buf, 1);
                if (buf[0] >= '0' && buf[0] <= '7') {
                    uint8_t to_sprite = buf[0] - 48;
                    if (SPRITE_SIZE_BYTES(to_sprite) == g_state.sprite_size_bytes) {
                        // Display error if bytes do not match
                        copy_sprite_data(SPRITE_DATA_ADDR(to_sprite));
                    } else {
                        bordercolor(COLOUR_RED);
                        cgetc();
                    }
                    bordercolor(DEFAULT_BORDER_COLOR);
                }
                break;

            case 118: // "V"-expand
                POKE(0xD017, PEEK(0xD017) ^ (1 << PREVIEW_SPRITE_NUM));
                FREEZE_POKE(
                    VIC_BASE + 0x17, FREEZE_PEEK(VIC_BASE + 0x17) ^ (1 << g_state.sprite_number));
                bordercolor(DEFAULT_BORDER_COLOR);
                update_sprite_preview();
                g_state.redraw_flags = REDRAW_SB_INFO;
                break;

            case 104: // "H"-expand
                POKE(0xD01D, PEEK(0xD01D) ^ (1 << PREVIEW_SPRITE_NUM));
                FREEZE_POKE(
                    VIC_BASE + 0x1D, FREEZE_PEEK(VIC_BASE + 0x1D) ^ (1 << g_state.sprite_number));
                bordercolor(DEFAULT_BORDER_COLOR);
                update_sprite_preview();
                g_state.redraw_flags = REDRAW_SB_INFO;
                break;

            case 8:  // CTRL-H, horizontal flip
            case 22: // CTRL-V, vertical flip
                // TODO: implement; issue #78
                break;

                /* ------------------------------- COLOR GROUP -------------------------- */

            case '+':
                g_state.redraw_flags = REDRAW_SB_COLOR;
                g_state.current_color_idx = (g_state.current_color_idx + 1) %
                    ((g_state.sprite_color_mode == SPR_COLOR_MODE_MONOCHROME |
                         g_state.sprite_color_mode == SPR_COLOR_MODE_16COLOR)
                            ? 2
                            : 4);
                break;

            case '-':
                g_state.redraw_flags = REDRAW_SB_COLOR;
                g_state.current_color_idx = (g_state.current_color_idx - 1) %
                    ((g_state.sprite_color_mode == SPR_COLOR_MODE_MONOCHROME |
                         g_state.sprite_color_mode == SPR_COLOR_MODE_16COLOR)
                            ? 2
                            : 4);
                break;

            case 107: // k
                set_background();
                break;

            case 16: // Ctrl+P
            {
                const unsigned char c_bank_reg = FREEZE_PEEK(REG_SPRPALSEL);
                const unsigned char c_spr_pal_bank = (c_bank_reg & 0xC) >> 2;
                gotoxy(1, 1);
                cputdec(c_spr_pal_bank, 0, 4);
                FREEZE_POKE(REG_SPRPALSEL, (c_bank_reg & ~0xC) | (((c_spr_pal_bank + 1) % 4) << 2));
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
                if (buf[0] == 'y' && buf[1] == 'e' && buf[2] == 's') {
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
                if (buf[0] == 'y' && buf[1] == 'e' && buf[2] == 's') {
                    clear_sprite();
                }
                break;

                // case ASC_HELP:
                //     break;

                /* --------------------------- DRAWING TOOLS GROUP ----------------------- */

                /*
                case 111: // o = circle  tool
                  SetDrawTool(DRAWING_TOOL_CIRCLE);
                  g_state.redrawFlags = REDRAW_SB_TOOLS | REDRAW_TOOL_PREVIEW;

                  SetRedrawFullCanvas();
                  break;

                case 79: // "O" = filled circle  tool
                  SetDrawTool(DRAWING_TOOL_FILLED_CIRCLE);
                  g_state.redrawFlags = REDRAW_SB_TOOLS | REDRAW_TOOL_PREVIEW;
                  SetRedrawFullCanvas();
                  break;
                */

            case 112: // p=pixel tool
                set_draw_tool(DRAWING_TOOL_PIXEL);
                g_state.redraw_flags = REDRAW_SB_TOOLS | REDRAW_TOOL_PREVIEW;
                set_redraw_full_canvas();
                g_state.tool_active = 0;
                break;

            case 120: // x = draw box
                set_draw_tool(DRAWING_TOOL_BOX);
                g_state.redraw_flags = REDRAW_SB_TOOLS | REDRAW_TOOL_PREVIEW;
                set_redraw_full_canvas();
                break;

            case 88: // "X"
                set_draw_tool(DRAWING_TOOL_FILLEDBOX);
                g_state.redraw_flags = REDRAW_SB_TOOLS | REDRAW_TOOL_PREVIEW;
                set_redraw_full_canvas();
                break;

            case 108: // l = line
                set_draw_tool(DRAWING_TOOL_LINE);
                g_state.redraw_flags = REDRAW_SB_TOOLS | REDRAW_TOOL_PREVIEW;
                set_redraw_full_canvas();
                break;

            default:
                if (key >= '0' && key <= '9') {
                    g_state.color[g_state.current_color_idx] = key - 48;
                    g_state.redraw_flags = REDRAW_SB_COLOR | REDRAW_TOOL_PREVIEW;
                    set_redraw_full_canvas();
                    update_color_regs();
                } else if (key >= 97 && key <= 102) // a..f
                {
                    g_state.color[g_state.current_color_idx] = 10 + key - 'a';
                    g_state.redraw_flags = REDRAW_SB_COLOR | REDRAW_TOOL_PREVIEW;
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
