/*
  Based on mega65-fdisk program as a starting point.

*/

#include "colours.h"
#include "common.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

#include <mega65.h>

static char freeze_menu_bar[] = "F3-RESUME    F5-RESET      HELP-MEGAINFO"
                                "F3-LOAD SLOT F7-SAVE SLOT  HELP-MEGAINFO";

/* Where each changing field sits.  The fixed text around them is a stream of
 * fragments in freezer/menu.cpp; these are the holes it leaves. */
static constexpr uint8_t KEY_BAR_X = 0, KEY_BAR_Y = 3;
static constexpr uint8_t SETTINGS_TOP_Y = 5; /* three rows: mode, frequency, video */
static constexpr uint8_t CPU_MODE_X = 13, CPU_FREQ_X = 13;
static constexpr uint8_t CRTEMU_MODE_X = 16, VIDEO_MODE_X = 33;
static constexpr uint8_t RIGHT_VALUE_X = 36; /* joystick swap, cartridge enable */
static constexpr uint8_t PROCESS_NAME_X = 21, PROCESS_NAME_Y = 14;
static constexpr uint8_t PROCESS_ROM_X = 26, PROCESS_ROM_Y = 15;
static constexpr uint8_t TASK_ID_X = 34, TASK_ID_Y = 16;
/* The slot label and the number in it are one field written in two parts. */
static constexpr uint8_t SLOT_LABEL_X = 20, SLOT_LABEL_Y = 17, SLOT_NUMBER_X = 34;
/* The two drives are three rows apart, which is what lets one pass serve both. */
static constexpr uint8_t DRIVE_NUM_X = 35, DRIVE_NUM_Y = 20;
static constexpr uint8_t D81_NAME_X = 22, D81_NAME_Y = 21;
static constexpr uint8_t DRIVE_ROW_STEP = 3;

static constexpr uint8_t ROOT_WARN_Y = 9;
static constexpr uint8_t ROOT_WARN_ROWS = 3;

/* Wide enough to blank the longest field drawn over. */
static const char BLANK_18[] = "                  ";

/* Three rows painted over the tools list, drawn by row rather than scanned to
 * a terminator. */
static const char ROOT_WARN[] = " NEED TO CHANGE CURRENT DIR TO ROOT TO  "
                                " START TOOL! THIS WILL BREAK DISK IMAGE "
                                " MOUNTS FROM SUBDIRS!    PROCEED (Y/N)? ";

// name of the file that is loaded by charset restore F14
#define DEFAULT_CHARSET "CHARSET.M65"

/* The charset of the ROM the machine is running, in chip RAM: the image sits at
 * $20000 and its charset $D000 into it.  $D67D.2 write-protects $20000-$3FFFF
 * and only latches in hypervisor mode, so nothing short of a program that asked
 * hyppo to unprotect it can have disturbed this. */
static constexpr Addr28 ROM_CHARSET = 0x2D000;

/* Screen code 0 is `@` in every C64-descended charset, and its first two rows
 * are enough to tell glyphs from the zeros or debris a failed ROM load would
 * leave.  The case guarded against is no ROM at all, not a differing one. */
static bool rom_charset_present(void) {
    return lpeek(ROM_CHARSET) == 0x3C && lpeek(ROM_CHARSET + 1) == 0x66;
}

static uint8_t rom_changed = 0;
bool in_subdirectory = false;

void to_petscii_upper(char* text, int length);

/* freezer/menu.cpp, the one C++ translation unit: the fixed text as a stream
 * of fragments, converted to screen codes when it was compiled. */
const uint8_t* menu_fixed_stream(void);

static uint8_t colour_table[256];

/* A thumbnail byte stores three bits of red, three of green and two of blue,
 * and palette entry N holds exactly the colour N names in that form, so a byte
 * is the number of its own entry.  Only the lowest sixteen need diverting: the
 * colour scheme owns those, and entry 0 is transparent besides. */
void make_colour_lookup(void) {
    uint8_t colour = 0;
    do {
        colour_table[colour] = colour;
    } while (++colour);

    /* One step up in red, which is as close to black as the entries above the
     * sixteen come; they share those entries with the shades just above. */
    for (colour = 0; colour < 16; colour++) {
        colour_table[colour] = colour | 0x20;
    }
}

/* The VIC-IV register block in the MEGA65 personality, which is where these
 * are written: the live chip, not the freeze slot. */
static constexpr Addr28 VIC4_REGISTERS = 0xFFD3000L;

/* Offsets into viciv_regs, which is indexed by the low byte of the register's
 * address.  Only the ones the code below reaches are named. */
static constexpr uint8_t VIC4_REG_KEY = 0x2F;
static constexpr uint8_t VIC4_REG_TOP_BORDER = 0x48;
static constexpr uint8_t VIC4_REG_BOTTOM_BORDER = 0x4A;
static constexpr uint8_t VIC4_REG_TEXT_YPOS = 0x4E;
static constexpr uint8_t VIC4_REG_CTRLC = 0x54;
static constexpr uint8_t VIC4_REG_RASLINE0 = 0x6F;
static constexpr uint8_t VIC4_REG_SPRITE_YADJ = 0x72;

/* Where the two standards differ.  The table cannot hold one answer for these,
 * so they are written over it once the current standard is known. */
static constexpr uint8_t NTSC_TOP_BORDER = 0x2A, NTSC_BOTTOM_BORDER = 0xB9;
static constexpr uint8_t NTSC_TEXT_YPOS = 0x2A, NTSC_SPRITE_YADJ = 0x18;
static constexpr uint8_t PAL_TOP_BORDER = 0x68, PAL_BOTTOM_BORDER = 0xF8;
static constexpr uint8_t PAL_TEXT_YPOS = 0x68, PAL_SPRITE_YADJ = 0x00;

/* The whole VIC-IV register block, $D000-$D07F, as the menu wants it.
 *
 * The freezer inherits whatever display the frozen program was using -- any
 * mode, any screen address, any raster split, sprites anywhere -- so it starts
 * from a block it wrote rather than from registers it would have to audit one
 * at a time.
 *
 * The values are what a working menu screen reads back, not a calculation, and
 * that is why the two standards are patched in afterwards instead of appearing
 * here: a table taken from one machine cannot state both. */
static uint8_t viciv_regs[0x80] = {
    /* Every entry not named here is zero, which is what the menu wants of
     * it: no sprites, no raster interrupt, no bitplanes. */
    [0x11] = 0x9B, /* YSCL      24/25 vertical smooth scroll */
    [0x12] = 0x37, /* RC        raster compare bits 0 to 7 */
    [0x16] = 0xC9, /* XSCL      horizontal smooth scroll */
    [0x18] = 0x14, /* CB        character set address location (x 1KiB) */
    [0x19] = 0x71, /* RIRQ      raster compare indicate or acknowledge */
    [0x1A] = 0xE0, /* MRIRQ     mask raster IRQ */
    [0x20] = 0x0E, /* BORDERCOL display border colour (256 colour) */
    [0x21] = 0x06, /* SCREENCOL screen colour (256 colour) */
    [0x22] = 0x01, /* MC1       multi-colour 1 (256 colour) */
    [0x23] = 0x02, /* MC2       multi-colour 2 (256 colour) */
    [0x24] = 0x03, /* MC3       multi-colour 3 (256 colour) */
    [0x25] = 0x04, /* SPRMC0    Sprite multi-colour 0 (8-bit for selection o */
    [0x27] = 0x01, /* SPR0COL   sprite N colour / 16-colour sprite transpare */
    [0x28] = 0x02, /* SPR1COL   @SPRNCOL */
    [0x29] = 0x03, /* SPR2COL   @SPRNCOL */
    [0x2A] = 0x04, /* SPR3COL   @SPRNCOL */
    [0x2B] = 0x05, /* SPR4COL   @SPRNCOL */
    [0x2C] = 0x06, /* SPR5COL   @SPRNCOL */
    [0x2D] = 0x07, /* SPR6COL   @SPRNCOL */
    [0x2E] = 0x0C, /* SPR7COL   @SPRNCOL */
    [0x31] = 0x20, /* VIC-III   Control Register B */
    [0x40] = 0xFF, /* B0PIX     Display Address Translater (DAT) Bitplane N  */
    [0x41] = 0xFF, /* B1PIX     @BNPIX */
    [0x42] = 0xFF, /* B2PIX     @BNPIX */
    [0x43] = 0xFF, /* B3PIX     @BNPIX */
    [0x44] = 0xFF, /* B4PIX     @BNPIX */
    [0x45] = 0xFF, /* B5PIX     @BNPIX */
    [0x46] = 0xFF, /* B6PIX     @BNPIX */
    [0x47] = 0xFF, /* B7PIX     @BNPIX */
    [0x48] = 0x68, /* TBDRPOS   top border position */
    [0x4A] = 0xF8, /* BBDRPOS   bottom border position */
    /* The MSB of the one above, and the same for both standards -- $01F8 and
     * $01B9 -- which is why only the LSB is patched for PAL and NTSC. */
    [0x4B] = 0x01, /* BBDRPOS   bottom border position, bits 8-11 */
    [0x4C] = 0x50, /* TEXTXPOS  character generator horizontal position */
    [0x4E] = 0x68, /* TEXTYPOS  Character generator vertical position */
    [0x50] = 0x0C, /* XPOSLSB   Read horizontal raster scan position LSB */
    [0x51] = 0x83, /* XPOSMSB   Read horizontal raster scan position MSB */
    [0x53] = 0x81, /* MSB       Read physical raster position */
    [0x54] = 0x05, /* VIC-IV    Control register C */
    [0x58] = 0x50, /* LINESTEPLSB number of bytes to advance between each text */
    [0x5A] = 0x78, /* CHRXSCL   Horizontal hardware scale of text mode (pixe */
    [0x5B] = 0x01, /* CHRYSCL   Vertical scaling of text mode (number of phy */
    [0x5C] = 0x50, /* LSB       Width of single side border (LSB) */
    [0x5D] = 0xC0, /* MSB       side border width (MSB) */
    [0x5E] = 0x28, /* CHRCOUNT  Number of characters to display per row (LSB */
    [0x61] = 0xB8, /* SCRNPTRMSB screen RAM precise base address (bits 15 - 8 */
    [0x67] = 0x1F, /* SBPDEBUG  Sprite/bitplane first X DEBUG WILL BE REMOVE */
    [0x69] = 0x90, /* CHARPTRMSB Character set precise base address (bits 15  */
    [0x6C] = 0xF8, /* SPRPTRADRLSB sprite pointer address (bits 7 - 0) */
    [0x6D] = 0x07, /* SPRPTRADRMSB sprite pointer address (bits 15 - 8) */
    [0x70] = 0xFF, /* VIC-IV    palette bank selection */
    [0x73] = 0x11, /* ALPHADELAY Alpha delay for compositor */
    [0x79] = 0x37, /* RASCMP    Physical raster compare value to be used if  */
    [0x7A] = 0x81, /* MSB       Raster compare value MSB */
    [0x7B] = 0x18, /* ROWS      Number of text rows to display */
    [0x7C] = 0xD2, /* PBANK     Set which 128KB bank bitplanes */
    [0x7F] = 0x7F, /* DEBUGX    VIC-IV debug X position (MSB) */
};

void setup_menu_screen(void) {
    /* Two settings are the user's rather than ours, so they are read back off
     * the chip and written into the block before it is sent. */
    viciv_regs[VIC4_REG_CTRLC] = (viciv_regs[VIC4_REG_CTRLC] & ~VIC4_CTRLC_CRT_EMULATION) |
        (VICIV.ctrlc & VIC4_CTRLC_CRT_EMULATION);
    viciv_regs[VIC4_REG_RASLINE0] = VICIV.rasline0;

    if (viciv_regs[VIC4_REG_RASLINE0] & VIC4_RASLINE0_NTSC) {
        viciv_regs[VIC4_REG_TOP_BORDER] = NTSC_TOP_BORDER;
        viciv_regs[VIC4_REG_BOTTOM_BORDER] = NTSC_BOTTOM_BORDER;
        viciv_regs[VIC4_REG_TEXT_YPOS] = NTSC_TEXT_YPOS;
        viciv_regs[VIC4_REG_SPRITE_YADJ] = NTSC_SPRITE_YADJ;
    } else {
        viciv_regs[VIC4_REG_TOP_BORDER] = PAL_TOP_BORDER;
        viciv_regs[VIC4_REG_BOTTOM_BORDER] = PAL_BOTTOM_BORDER;
        viciv_regs[VIC4_REG_TEXT_YPOS] = PAL_TEXT_YPOS;
        viciv_regs[VIC4_REG_SPRITE_YADJ] = PAL_SPRITE_YADJ;
    }

    /* In two halves with $D02F left out: it is the I/O personality knock, and
     * viciv.vhdl reverts to VIC-II unless a whole knock sequence completes, so
     * writing a stale value there drops the display out from under us.  Both
     * lengths come from that one register number, so the hole cannot drift. */
    lcopy((Addr28)viciv_regs, VIC4_REGISTERS, VIC4_REG_KEY);
    lcopy((Addr28)(viciv_regs + VIC4_REG_KEY + 1),
        VIC4_REGISTERS + VIC4_REG_KEY + 1,
        sizeof viciv_regs - VIC4_REG_KEY - 1);

    clear_colour_ram();
}

uint8_t next_cpu_speed(void) {
    switch (detect_cpu_speed()) {
        case 1:
            // Make it 2MHz: 2MHZ && !FAST && !VFAST
            // ffd0030 is a special register to access the C128 D030.0 bit
            freeze_io_poke(0x0030, 1);
            freeze_io_update(0x3031, 0xbf, 0x00);
            freeze_io_update(0x3054, 0xbf, 0x00);
            return 1;
        case 2:
            // Make it 3.5MHz: !2MHZ && FAST && !VFAST
            freeze_io_poke(0x0030, 0);
            freeze_io_update(0x3031, 0xff, 0x40);
            freeze_io_update(0x3054, 0xbf, 0x00);
            break;
        case 3:
            // Make it 40MHz: !2MHZ && FAST && VFAST
            freeze_io_poke(0x0030, 0);
            freeze_io_update(0x3031, 0xff, 0x40);
            freeze_io_update(0x3054, 0xff, 0x40);
            break;
        case 40:
        default:
            // Make it 1MHz: !2MHZ && !FAST && !VFAST
            freeze_io_poke(0x0030, 0);
            freeze_io_update(0x3031, 0xbf, 0x00);
            freeze_io_update(0x3054, 0xbf, 0x00);
            // If a program forced 40 MHz via POKE 0,65, the Hypervisor flag at
            // ffd367d is set, and is overriding the other flags. Clear it.
            // The Freezer itself does not set this.
            freeze_io_update(0x367d, 0xef, 0x00);
            return 1;
    }
    return 0;
}

/* src/link.ld puts .thumbnail above the region the linker allocates from, so
 * that this does not come out of the same budget as the code. */
static __attribute__((section(".thumbnail"))) uint8_t thumbnail_buffer[4096];

/* White under xemu, which has no thumbnail generator: $FFD4000 reads as $FF,
 * freezes that way, and $FF is the palette's brightest entry.  Black instead
 * means nothing was frozen -- zero maps to the near-black entry just above the
 * sixteen.  Neither says anything about this code; on hardware it is the
 * frozen screen. */
void draw_thumbnail(void) {
    /* Render the slot's 4KB thumbnail to the tile data at $50000.
     *
     * The region is found directly rather than through
     * address_to_freeze_slot_offset(): the thumbnail's nominal address of
     * $0010000 overlaps main RAM, so the usual lookup would find the wrong
     * region.  Its bytes are linear where the tiles are not, hence the
     * rearrange pass below -- done through a buffer, because a per-pixel
     * lpoke() is one DMA job each and the slot browser has to flip smoothly. */
    uint32_t thumbnail_sector = find_thumbnail_offset();

    // Can't find thumbnail area?  Then show no thumbnail
    if (thumbnail_sector == FREEZE_SLOT_NOT_PRESENT) {
        lfill(0x50000L, 0, 10 * 6 * 64);
        return;
    }
    // Copy thumbnail memory to buffer
    for (uint8_t i = 0; i < 8; i++) {
        sdcard_readsector(freeze_slot_start_sector + thumbnail_sector + i);
        lcopy((Addr28)sector_buffer, (Addr28)(thumbnail_buffer + (i * 0x200)), 0x200);
        NAVIGATION_KEY_CHECK();
    }

    // Pick colours of all pixels in the thumbnail
    for (uint16_t j = 0; j < 4096; j++) {
        thumbnail_buffer[j] = colour_table[thumbnail_buffer[j]];
    }

    /* Fix column 0 of pixels.  The offset runs alongside the counter rather
     * than being multiplied out of it: a multiply per row costs a call here. */
    uint16_t row_start = 0;
    for (uint16_t row = 0; row < 49; row++) {
        thumbnail_buffer[row_start] = thumbnail_buffer[row_start + 1];
        row_start += 80;
    }

    // Rearrange pixels
    uint16_t yoffset = 80 + 13; // skip dud first line
    for (uint8_t y = 0; y < 48; y++) {
        uint16_t yoffset_out = ((y & 7) << 3) + (y >> 3) * 64;
        uint16_t xoffset = 0;
        for (uint8_t x = 0; x < 73; x += 8) {
            // Also the whole thing is rotated by one byte, so add that on as we plot the pixel
            // PGS Optimise here

            /* The last column is a part row: 73 pixels do not divide by eight. */
            uint16_t count = (x == 72) ? 2 : 8;

            lcopy((uint32_t)&thumbnail_buffer[x + yoffset],
                0x50000L + (xoffset + yoffset_out),
                count);

            xoffset += 64 * 6;
        }
        NAVIGATION_KEY_CHECK();
        yoffset += 80;
    }
}

static struct ProcessDescriptor process_descriptor;

static int8_t last_thumb_frame = -1;
/* Whether $52000 holds a frame at all.  Separate from last_thumb_frame, which
 * says what is on the screen and is invalidated to force a redraw. */
static bool frame_loaded = false;
static uint8_t thumb_xoff = 5, thumb_yoff = 1;
/* Index into thumb_frame_name[]. */
enum ThumbFrame : uint8_t {
    ThumbFrameM65 = 0,
    ThumbFrameC65 = 1,
    ThumbFrameC64 = 2,
};
/* Padded to a power of two: a 13-byte stride makes indexing a multiply, which
 * on this target is a call to __mulhi3 and links the whole helper for one use. */
// clang-format off
static char thumb_frame_name[][16] = {
    "M65THUMB.M65",
    "C65THUMB.M65",
    "C64THUMB.M65"
};
// clang-format on

/* Blanks a whole row, both planes.  The fixed parts are runs of text with gaps
 * between them, so anything drawn over a whole row has to be cleared rather
 * than merely drawn over. */
static void clear_menu_row(uint8_t y) {
    const uint16_t cell = SCREEN_CELL(0, y);
    lfill_skip(SCREEN_ADDRESS + cell, ' ', 40, SCREEN_CELL_BYTES);
    lfill_skip(
        COLOUR_RAM_ADDRESS + cell + (SCREEN_CELL_BYTES - 1), SchemeText, 40, SCREEN_CELL_BYTES);
}

/* Everything that does not change: the stream from freezer/menu.cpp, plus the
 * rules.  Its own entry point because the root-directory warning paints over
 * rows 9-11 and must restore them when dismissed. */
void draw_menu_fixed(void) {
    draw_fragments(menu_fixed_stream());
    draw_rule(SCREEN_CELL(0, 2), 40);
    draw_rule(SCREEN_CELL(0, 4), 40);
    draw_rule(SCREEN_CELL(0, 8), 40);
    draw_rule(SCREEN_CELL(0, 12), 40);
}

void predraw_freeze_menu(void) {
    // Clear screen, blue background, white text, like Action Replay
    VICIV.bordercol = SchemeBorder;
    VICIV.screencol = SchemeBackground;

    clear_colour_ram();
    blank_screen();
    draw_menu_fixed();

    last_thumb_frame = -1;
}

/* Which parts of the menu draw_freeze_menu() redraws, OR-ed together. */
enum : uint8_t {
    UpdateTop = 0x01,
    UpdateFreq = 0x02,
    UpdateProcess = 0x04,
    UpdateDisk = 0x08,
    UpdateThumb = 0x10,
    /* Derived, so deleting a field cannot leave it claiming a bit. */
    UpdateAll = UpdateTop | UpdateFreq | UpdateProcess | UpdateDisk | UpdateThumb,
    UpdateChgSlot = 0x20,
};

void draw_freeze_menu(uint8_t part) {
    uint16_t i;
    uint8_t x;
    uint8_t y;

    if (part & UpdateChgSlot) {
        freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);
    }

    // Update messages based on the settings we allow to be easily changed
    if (part & UpdateTop) {

        if (slot_number) {
            draw_text(SCREEN_CELL(KEY_BAR_X, KEY_BAR_Y), SchemeText, freeze_menu_bar + 40, 40);
            draw_text(
                SCREEN_CELL(SLOT_LABEL_X, SLOT_LABEL_Y), SchemeTextDim, " FREEZE SLOT:      ", 19);
            draw_decimal(SCREEN_CELL(SLOT_NUMBER_X, SLOT_LABEL_Y), SchemeValue, slot_number);
        } else {
            draw_text(SCREEN_CELL(KEY_BAR_X, KEY_BAR_Y), SchemeText, freeze_menu_bar, 40);
            if (rom_changed) {
                /* Blanking nine cells is a fill; draw_text would fold nine
                 * spaces, which converts each to itself. */
                lfill_skip(
                    SCREEN_ADDRESS + SCREEN_CELL(KEY_BAR_X, KEY_BAR_Y), ' ', 9, SCREEN_CELL_BYTES);
            }

            draw_text(
                SCREEN_CELL(SLOT_LABEL_X, SLOT_LABEL_Y), SchemeValue, " - PAUSED STATE -   ", 19);
        }

        // CPU MODE
        draw_text(SCREEN_CELL(CPU_MODE_X, SETTINGS_TOP_Y),
            SchemeValue,
            (freeze_io_peek(0x367d) & 0x20) ? "  4502" : "  AUTO",
            6);

        // Joystick 1/2 swap
        draw_text(SCREEN_CELL(RIGHT_VALUE_X, SETTINGS_TOP_Y),
            SchemeValue,
            (UART_MISC & UART_MISC_JOYSWAP) ? "YES" : " NO",
            3);

        // Cartridge enable
        draw_text(SCREEN_CELL(RIGHT_VALUE_X, SETTINGS_TOP_Y + 1),
            SchemeValue,
            (freeze_io_peek(0x367d) & 0x01) ? "YES" : " NO",
            3);

        // PALEMU
        draw_text(SCREEN_CELL(CRTEMU_MODE_X, SETTINGS_TOP_Y + 2),
            SchemeValue,
            (freeze_io_peek(0x3054) & 0x20) ? " ON" : "OFF",
            3);

        draw_text(SCREEN_CELL(VIDEO_MODE_X, SETTINGS_TOP_Y + 2),
            SchemeValue,
            (freeze_io_peek(0x306f) & 0x80) ? "NTSC60" : " PAL50",
            6);
    }

    // CPU frequency
    if (part & UpdateFreq) {
        const char* speed;
        switch (detect_cpu_speed()) {
            case 1:
                speed = "  1";
                break;
            case 2:
                speed = "  2";
                break;
            case 3:
                speed = "3.5";
                break;
            case 40:
                speed = " 40";
                break;
            default:
                speed = "???";
                break;
        }
        draw_text(SCREEN_CELL(CPU_FREQ_X, SETTINGS_TOP_Y + 1), SchemeValue, speed, 3);
    }

    if ((part & UpdateProcess) || (part & UpdateThumb)) {
        detect_rom();
    }

    /* Display info from the process descriptor
       The useful bits are:
       $00     - Task ID (0-255, $FF = operating system)
       $01-$10 - Process name (16 characters)
       $11     - D81 image 0 flags
       $12     - D81 image 1 flags
       $13     - D81 image 0 name len
       $14     - D81 image 1 name len
       $15-$34 - D81 image 0 file name (max 32 chars, not null terminated)
       $35-$54 - D81 image 0 file name (max 32 chars, not null terminated)
       $55-$7F - RESERVED
       $80-$FF - File descriptors

       We should just read the sector containing all this, and get it out all at once.
    */
    if ((part & UpdateProcess) || (part & UpdateDisk)) {
        freeze_fetch_sector(0xFFFBD00L, (unsigned char*)&process_descriptor);
    }

    if (part & UpdateProcess) {
        draw_decimal(SCREEN_CELL(TASK_ID_X, TASK_ID_Y), SchemeValue, process_descriptor.task_id);

        // Process name: only display if no unprintable PETSCII chars
        for (i = 0; i < 16; i++) {
            if ((process_descriptor.process_name[i] & 0x7f) < 0x20) {
                break;
            }
        }
        draw_text(SCREEN_CELL(PROCESS_NAME_X, PROCESS_NAME_Y),
            SchemeValue,
            (i == 16) ? process_descriptor.process_name : "UNNAMED TASK    ",
            16);

        draw_text(SCREEN_CELL(PROCESS_ROM_X, PROCESS_ROM_Y), SchemeValue, mega65_rom_name, 11);
    }

    if (part & UpdateDisk) {
        /* Hyppo keeps the two drives' fields in arrays, so one pass serves both. */
        for (uint8_t drive = 0; drive < 2; drive++) {
            const uint16_t number_cell =
                SCREEN_CELL(DRIVE_NUM_X, DRIVE_NUM_Y + drive * DRIVE_ROW_STEP);
            const uint16_t name_cell = SCREEN_CELL(D81_NAME_X, D81_NAME_Y + drive * DRIVE_ROW_STEP);
            const uint8_t flags = process_descriptor.d81_flags[drive];
            const uint8_t namelen = process_descriptor.d81_namelen[drive];
            char* name = process_descriptor.d81_name[drive];

            draw_decimal(number_cell, SchemeValue, freeze_peek(0x10113L + drive));

            /* The name is drawn over, not into a cleared buffer, so it needs a
             * blank first: a shorter name would leave a longer one's tail. */
            draw_text(name_cell, SchemeAccent, BLANK_18, 18);

            if ((flags & PdImgFlagsMounted) && namelen) {
                /* A NUL inside the name means hyppo did not write it. */
                for (i = 0; i < namelen && name[i]; i++) {
                }
                if (i == namelen) {
                    to_petscii_upper(name, namelen);
                    draw_text(name_cell, SchemeAccent, name, namelen < 18 ? namelen : 18);
                }
            } else if (flags & PdImgFlagsNoReal) {
                draw_text(name_cell, SchemeAccent, NO_DISK_DRIVE, sizeof(NO_DISK_DRIVE) - 1);
            } else if (drive) {
                draw_text(name_cell, SchemeAccent, INTERNAL_DRIVE_1, sizeof(INTERNAL_DRIVE_1) - 1);
            } else {
                draw_text(name_cell, SchemeAccent, INTERNAL_DRIVE_0, sizeof(INTERNAL_DRIVE_0) - 1);
            }
        }
    }

    // Draw the thumbnail surround area
    if (part & UpdateThumb) {
        /* Wait for the raster to leave the screen: only the thumbnail is large
         * enough to tear, and the settings keys redraw far more often. */
        while (VICIV.rasterline < 0xf8) {
        }

        int8_t thumb_frame = ThumbFrameM65;

        switch (mega65_rom_type) {
            case Mega65RomC64:
                thumb_frame = ThumbFrameC64;
                break;
            case Mega65RomC65:
                thumb_frame = ThumbFrameC65;
                break;
            case Mega65RomM65:
                if (detect_cpu_speed() == 1) {
                    thumb_frame = ThumbFrameC64;
                } else {
                    thumb_frame = ThumbFrameM65;
                }
                break;
            case Mega65RomOpenRom:
            default:
                thumb_frame = ThumbFrameM65;
                break;
        }

        /* Only from the root: hyppo opens by name relative to the current
         * directory, and the frame files are not in a subdirectory. */
        if (!in_subdirectory && thumb_frame != last_thumb_frame) {
            while (thumb_frame > -1) {
                if (!read_file_from_sdcard(thumb_frame_name[thumb_frame], 0x052000L)) {
                    frame_loaded = true;
                    break;
                }
                // fall through to next lower thumb image
                thumb_frame--;
            }
        }

        /* Work out where the tile data begins.  Guarded by frame_loaded
         * because the read above is skipped in a subdirectory, and the tile
         * numbers and offsets would otherwise come from whatever the frozen
         * program left at $52000. */
        if (frame_loaded && thumb_frame > -1 && thumb_frame != last_thumb_frame) {
            uint32_t screen_data_start;
            uint16_t* tile_num;

            screen_data_start = 0x52000L + 0x300L + 0x40L;
            uint16_t tile_offset = (uint16_t)(screen_data_start >> 6);
            // Work out where the screen data begins
            screen_data_start = lpeek(0x5203dL) + (lpeek(0x5203eL) << 8);
            screen_data_start += 0x52000L + 0x40L;
            /* Rows 13 to 24 is twelve, not thirteen: a thirteenth pass starts
             * at $BFD0, the first byte past the 2000-byte screen. */
            for (y = 0; y < 12; y++) {
                // Copy row of screen data
                lcopy(screen_data_start + (y << 6),
                    SCREEN_ADDRESS + (13 * SCREEN_ROW_BYTES) + (y * SCREEN_ROW_BYTES),
                    (19 * 2));
                // Add tile number based on data starting at $52040 = $1481
                for (x = 0; x < 19; x++) {
                    tile_num = (uint16_t*)(SCREEN_ADDRESS + (13 * SCREEN_ROW_BYTES) +
                        (y * SCREEN_ROW_BYTES) + (x << 1));
                    if (*tile_num) {
                        (*tile_num) += tile_offset;
                    } else {
                        *tile_num = 0x20;
                    }
                }
            }
            thumb_xoff = lpeek(0x52020L);
            thumb_yoff = thumb_xoff >> 4;
            thumb_xoff &= 0xf;
            last_thumb_frame = thumb_frame;
        } else if (thumb_frame == -1) {
            thumb_xoff = 5;
            thumb_yoff = 2;
            last_thumb_frame = -1;
        }

        // Now draw the 9x6 character block for thumbnail display itself
        // This sits in the region below the menu where we will also have left and right arrows,
        // the program name etc, so you can easily browse through the freeze slots.
        draw_thumbnail();
        /* Rows advance by adding: a row term inside the loop would be a 16-bit
         * multiply per cell, which the 6502 pays for with a call. */
        uint16_t row = SCREEN_ADDRESS + SCREEN_CELL(thumb_xoff, 13 + thumb_yoff);
        for (y = 0; y < 6; y++) {
            for (x = 0; x < 9; x++) {
                const uint16_t cell = row + (uint16_t)(x * SCREEN_CELL_BYTES);
                POKE(cell + 0, x * 6 + y); // $50000 base address
                POKE(cell + 1, 0x14);
            }
            row += SCREEN_ROW_BYTES;
        }
    }

    // restore border colour (the SD card code still twiddles with it)
    VICIV.bordercol = SchemeBorder;
}

void to_petscii_upper(char* text, int length) {
    for (int i = 0; i < length; i++) {
        if (text[i] >= 0x60 && text[i] < 0x7a) {
            text[i] &= 0x5f;
        }
    }
}

void change_mounted_disk_image(uint8_t diskid) {
    char* ret;
    ret = freeze_select_disk_image(diskid);
    if (ret) {
        if (hdos_new_attach) {
            copy_imageproc_to_freezeregion(diskid, 0);
        } else {
            old_store_selected_disk_image(diskid, ret);
        }
    }

    predraw_freeze_menu();
    draw_freeze_menu(UpdateAll | UpdateChgSlot);
}

/* Flags for fix_chargen_area(), OR-ed together. */
enum : uint8_t {
    ChargenFixMem = 0x01,  // write char data to chargen memory
    ChargenFixSlot = 0x02, // write char data to slot storage
    ChargenForce = 0x40,   // if check can't load region, do fix anyway
    ChargenNoCheck = 0x80, // don't execute check, always fix
};
void fix_chargen_area(uint8_t flags) {
    uint16_t i = SD_SECTOR_SIZE; // needs to be SD_SECTOR_SIZE for nocheck to trigger!
    long charset_start;

    if (!(flags & ChargenNoCheck)) {
        if (freeze_fetch_sector(CHARGEN_ADDRESS, NULL) == FreezerOk) {
            // check if everything is zero
            for (i = 0; i < SD_SECTOR_SIZE && !sector_buffer[i]; i++) {
                ;
            }
        } else {
            // error while reading sector (old core?)
            i = (flags & ChargenForce) ? SD_SECTOR_SIZE : 0;
        }
    }

    // if first chargen sector was zero...
    if (i == SD_SECTOR_SIZE) {
        charset_start = -1;
        /* Hyppo loads MEGA65.ROM to $20000 immediately before it loads this
         * program (mega65-core src/hyppo/task.asm, attempt_loadc65rom), so the
         * charset is already in RAM.  Reading that same file back off the card
         * fetched 128K to use 4K of it, which is what the card is left holding
         * only when the ROM did not load at all. */
        if (rom_charset_present()) {
            charset_start = ROM_CHARSET;
        } else if (!read_file_from_sdcard(DEFAULT_CHARSET, 0x40000L)) {
            charset_start = 0x40000L;
        }

        if (charset_start != -1) {
            // copy the font to chargen WOM directly
            if (flags & ChargenFixMem) {
                lcopy(charset_start, CHARGEN_ADDRESS, 4096);
            }

            /* The slot as well, so the charset survives the resume rather
             * than only looking right while the menu is up. */
            if (flags & ChargenFixSlot) {
                for (i = 0; i < 8; i++) {
                    lcopy(charset_start + (long)SD_SECTOR_SIZE * i,
                        (long)sector_buffer,
                        SD_SECTOR_SIZE);
                    freeze_store_sector(CHARGEN_ADDRESS + (long)SD_SECTOR_SIZE * i, NULL);
                }
            }
        } else {
            // failed to load font, flash screen
            VICIV.bordercol = SchemeError;
            VICIV.screencol = SchemeError;
            usleep(150000L);
            VICIV.bordercol = SchemeBorder;
            VICIV.screencol = SchemeBackground;
        }
    }
}

void start_freezer_tool(char* toolfile) {
    char x = 0;
    char start_tool = 0;

    if (in_subdirectory) {
        for (uint8_t row = 0; row < ROOT_WARN_ROWS; row++) {
            draw_text(SCREEN_CELL(0, ROOT_WARN_Y + row), SchemeWarning, &ROOT_WARN[row * 40], 40);
        }

        while (!start_tool) {
            while (!(x = ASCIIKEY)) {
                ;
            }
            ASCIIKEY = 0;
            switch (x) {
                case 'y':
                case 'Y':
                    mega65_dos_cdroot();
                    start_tool = 1;
                    break;
                case 'n':
                case 'N':
                case KEY_ESC:
                case KEY_RUN_STOP:
                    /* The warning covers whole rows; the fixed parts only
                     * cover their own runs, so blank the rows before redrawing
                     * or the gaps between fragments keep the warning's text. */
                    for (uint8_t row = 0; row < ROOT_WARN_ROWS; row++) {
                        clear_menu_row(ROOT_WARN_Y + row);
                    }
                    draw_menu_fixed();
                    draw_freeze_menu(UpdateTop);
                    return;
                default:
                    break;
            }
        }
    }
    mega65_dos_exechelper(toolfile);
}

int main(void) {
    uint8_t drive_state;
    uint8_t image_state;
    /* Performs the $D02F knock; without it every later write to a VIC-IV
     * register such as $D054 is silently ignored and the screen mode is
     * never established. */
    mega65_fast();

    // Disable interrupts and interrupt sources
    init_nmi(); // this also does SEI
    CIA1.icr = 0x7F;
    CIA2.icr = 0x7F;
    VICIV.imr = 0x00;
    /* The two CIAs and the VIC only: the C65 UART and the ethernet controller
     * can still raise an interrupt here. */

    // check border for return codes from other helpers
    if (VICIV.bordercol == BORDER_SIGNAL_ROM_CHANGED) {
        rom_changed = 1;
    }

    // Bank out BASIC ROM, leave KERNAL and IO in
    CPU_PORTDDR = CPU_PORT_DDR_ALL_OUTPUTS;
    CPU_PORT = CPU_PORT_KERNAL_AND_IO;

    // Disable Cartridge ROM
    lpoke(0xFFD37FDL, lpeek(0xFFD37FDL) | 0xC0); // Ensure forced exrom & game are high=disabled
    lpoke(0xFFD37FBL,
        lpeek(0xFFD37FBL) &
            0xFD); // Disable cartridge (core will use forced values above for exrom/game)

    // No decimal mode!
    __asm__ volatile("cld");

    // Put $DD00 DDR back to default
    CIA2.ddra = 0xFF;

    // Silence SIDs
    SID1.amp = 0;
    SID2.amp = 0;
    SID3.amp = 0;
    SID4.amp = 0;

    set_palette();
    make_colour_lookup();

    // assure we're viewing the sdcard's sector buffer (and not the floppy disk buffer)
    uint8_t orig_sd_misc = SD_MISC;
    SD_MISC = SD_MISC | SD_MISC_BUFSEL_SDCARD;

    // Now find the start sector of the slot, and make a copy for safe keeping
    slot_number = 0;
    freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);

    // SD or SDHC card?
    if (SD_STATUS & SD_STATUS_SDHC) {
        sdhc_card = 1;
    } else {
        sdhc_card = 0;
    }

    request_freeze_region_list();

    // initialize helper HDOS compability
    mega65_dos_init();

    // BASIC65 unmount will just poke D6A1, and
    // not use hyppo, because we don't have a function
    // for that! so we need to udpate the process
    // descriptor to show that we have the internal
    // drive mounted
    if (hdos_new_attach) {
        drive_state = lpeek(0xFFD36A1);
        if (drive_state & 0x1) {
            copy_imageproc_to_freezeregion(0, 1);
        }
        if (drive_state & 0x2) {
            copy_imageproc_to_freezeregion(1, 1);
        }
    } else {
        // for old HDOS < 1.3 we need to fix image RW flag,
        // and override the flags for drive / no disk
        drive_state = lpeek(0xFFD36A1);
        image_state = lpeek(0xFFD368B);
        copy_imageproc_to_freezeregion(0, image_state & 1 ? 0 : (drive_state & 1 ? 1 : 2));
        copy_imageproc_to_freezeregion(1, image_state & 8 ? 0 : (drive_state & 2 ? 1 : 2));
    }

    setup_menu_screen();
    predraw_freeze_menu();
    fix_chargen_area(ChargenFixMem | ChargenNoCheck);
    draw_freeze_menu(UpdateAll);

    // Flush input buffer
    while (ASCIIKEY) {
        ASCIIKEY = 0;
    }

    // Ensure correct keyboard DDR etc
    CIA1.pra = 0xFF;
    CIA1.ddra = 0x00;

    // Main keyboard input loop
    while (1) {
        uint8_t key = ASCIIKEY;

        if (key) {
            // Flush char from input buffer
            ASCIIKEY = 0;
        }

        // Process char
        if (key) {
            switch (key) {
                case KEY_HOME: // Home
                    if (slot_number) {
                        slot_number = 0;

                        draw_freeze_menu(
                            UpdateTop | UpdateProcess | UpdateDisk | UpdateThumb | UpdateChgSlot);
                    }
                    break;
                case ',':
                    slot_number -= 90;
                case KEY_CURSOR_DOWN: // Cursor down
                    slot_number -= 9;
                case KEY_CURSOR_LEFT: // Cursor left
                    slot_number--;
                    if (slot_number >= get_freeze_slot_count()) { // unsigned!
                        slot_number = get_freeze_slot_count() - 1;
                    }

                    draw_freeze_menu(
                        UpdateTop | UpdateProcess | UpdateDisk | UpdateThumb | UpdateChgSlot);
                    break;
                case '.':
                    slot_number += 90;
                case KEY_CURSOR_UP: // Cursor up
                    slot_number += 9;
                case KEY_CURSOR_RIGHT: // Cursor right
                    slot_number++;
                    if (slot_number >= get_freeze_slot_count()) {
                        slot_number = 0;
                    }

                    draw_freeze_menu(
                        UpdateTop | UpdateProcess | UpdateDisk | UpdateThumb | UpdateChgSlot);
                    break;

                case 'M':
                case 'm': // Monitor
                    start_freezer_tool("MONITOR.M65");
                    break;

                case 'A':
                case 'a': // Audio mixer
                    start_freezer_tool("AUDIOMIX.M65");
                    break;

                case 'S':
                case 's': // Sprite Editor
                    start_freezer_tool("SPRITED.M65");
                    break;

                case 'D':
                case 'd': // FileHost browser
                    start_freezer_tool("FILEHOST.M65");
                    break;

                case 'J':
                case 'j': // Toggle joystick swap
                    UART_MISC = (UART_MISC ^ UART_MISC_JOYSWAP) & (uint8_t)~UART_MISC_OSKDEBUG;

                    draw_freeze_menu(UpdateTop);
                    break;

                case 'T':
                case 't': // Toggle cartridge enable
                    freeze_io_poke(0x367d, freeze_io_peek(0x367d) ^ 0x01);
                    draw_freeze_menu(UpdateTop);
                    break;

                case 'c':
                case 'C': // Toggle CPU mode
                    freeze_io_poke(0x367d, freeze_io_peek(0x367d) ^ 0x20);
                    draw_freeze_menu(UpdateTop);
                    break;

                case 'F':
                case 'f': // Change CPU speed
                    if (next_cpu_speed()) {
                        draw_freeze_menu(UpdateFreq | UpdateThumb);
                    } else {
                        draw_freeze_menu(UpdateFreq);
                    }
                    break;

                case 'V':
                case 'v': // Toggle video mode
                    // Toggle video mode setting
                    // Then also toggle vertical border and text/graphics area positions, by
                    // updating the following: $FFD3048 = LSB, top border position $FFD3049.0-3 =
                    // MSB, top border position $FFD3049.7-4 = PRESERVE $FFD304A = LSB, bottom
                    // border position $FFD304B.0-3 = MSB, bottom border position $FFD304B.7-4 =
                    // PRESERVE $FFD304E = TEXTYPOS LSB $FFD304F.0-3 = TEXTYPOS MSB $FFD304F.4-7 =
                    // PRESERVE $FFD306F.0-5 = VIC-II first raster $FFD3072     = Sprite Y position
                    // adjust
                    key = freeze_io_peek(0x306f) & 0x80;
                    if (key == 0x80) {
                        // Switch to PAL
                        freeze_io_poke(0x306f, 0x00);
                        freeze_io_poke(0x3072, 0x00);
                        freeze_io_poke(0x3048, 0x68);
                        freeze_io_update(0x3049, 0xf0, 0x00);
                        freeze_io_poke(0x304a, 0xF8);
                        freeze_io_update(0x304b, 0xf0, 0x1);
                        freeze_io_poke(0x304e, 0x68);
                        freeze_io_update(0x304f, 0xf0, 0x00);
                        // CIA TOD
                        freeze_io_update(0x3c0e, 0xff, 0x80);
                        freeze_io_update(0x3d0e, 0xff, 0x80);
                        // do it for the freezer itself
                        VICIV.rasline0 = 0x00;
                        VICIV.spr_yadj = 0x00;
                        VICIV.tbdrpos_lsb = 0x68;
                        VICIV.tbdrpos_msb = (VICIV.tbdrpos_msb & 0xf0);
                        VICIV.bbdrpos_lsb = 0xF8;
                        VICIV.bbdrpos_msb = 0x1 | (VICIV.bbdrpos_msb & 0xf0);
                        VICIV.textypos_lsb = 0x68;
                        VICIV.textypos_msb = (VICIV.textypos_msb & 0xf0);
                        // CIA TOD
                        CIA1.cra = CIA1.cra | 0x80;
                        CIA2.cra = CIA2.cra | 0x80;
                    } else {
                        // Switch to NTSC
                        freeze_io_poke(0x306f, 0x87);
                        freeze_io_poke(0x3072, 0x18);
                        freeze_io_poke(0x3048, 0x2A);
                        freeze_io_update(0x3049, 0xf0, 0x00);
                        freeze_io_poke(0x304a, 0xB9);
                        freeze_io_update(0x304b, 0xf0, 0x1);
                        freeze_io_poke(0x304e, 0x2A);
                        freeze_io_update(0x304f, 0xf0, 0x00);
                        // CIA TOD
                        freeze_io_update(0x3c0e, 0x7f, 0x00);
                        freeze_io_update(0x3d0e, 0x7f, 0x00);
                        // do it for the freezer itself
                        VICIV.rasline0 = 0x87;
                        VICIV.spr_yadj = 0x18;
                        VICIV.tbdrpos_lsb = 0x2A;
                        VICIV.tbdrpos_msb = (VICIV.tbdrpos_msb & 0xf0);
                        VICIV.bbdrpos_lsb = 0xB9;
                        VICIV.bbdrpos_msb = 0x1 | (VICIV.bbdrpos_msb & 0xf0);
                        VICIV.textypos_lsb = 0x2A;
                        VICIV.textypos_msb = (VICIV.textypos_msb & 0xf0);
                        // CIA TOD
                        CIA1.cra = CIA1.cra & 0x7f;
                        CIA2.cra = CIA2.cra & 0x7f;
                    }
                    draw_freeze_menu(UpdateTop);
                    break;

                case '8':
                case '9':
                    // Change drive number of internal drives
                    freeze_poke(0x10113L - '8' + key, freeze_peek(0x10113L - '8' + key) ^ 2);
                    draw_freeze_menu(UpdateDisk);
                    break;
                case '0': // Select mounted disk image
                    change_mounted_disk_image(0);
                    break;
                case '1': // Select mounted disk image for 2nd drive
                    change_mounted_disk_image(1);
                    break;

                case 'P':
                case 'p': // next colour scheme
                    apply_scheme((uint8_t)(current_scheme + 1));
                    break;

                case KEY_F5: // F5 = Reset
                    // reset only works for slot 0!
                    if (slot_number != 0) {
                        goto invalid_function;
                    }
                    freeze_reset_cpu_state();
                    // fall through
                case KEY_F3: // F3 = resume
                case KEY_F4: // RESUME even if ROM changed
                    // if rom changed, slot 0 resume is disabled, reset is required
                    if (key == KEY_F3 && slot_number == 0 && rom_changed) {
                        goto invalid_function;
                    }
                    // Doesn't seem to really help (probably needs to be done by the hypervisor
                    // unfreezing routine?)
                    SD_MISC = orig_sd_misc;

                    // workaround for old freeze slots that have an empty chargen area
                    fix_chargen_area(ChargenFixMem | ChargenFixSlot);

                    unfreeze_slot(slot_number);

                    // should never get here
                    screen_of_death("unfreeze failed");

                    break;

                case KEY_F7: // F7 = save to slot
                {
                    uint32_t dest_freeze_slot_start_sector;

                    // can't save to slot 0
                    if (slot_number == 0) {
                        VICIV.bordercol = SchemeError;
                        VICIV.screencol = SchemeError;
                        usleep(150000L);
                        VICIV.bordercol = SchemeBorder;
                        VICIV.screencol = SchemeBackground;
                        continue;
                    }

                    // Get start sectors of the source and destination slots
                    freeze_slot_start_sector = read_freeze_slot_start_sector(0);
                    dest_freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);

                    // 512KB = 1024 sectors
                    // Process in 64KB blocks, so that we can do multi-sector writes
                    // and generally be about 10x faster than otherwise.
                    for (uint32_t i = 0; i < 1024; i += 128) {
                        VICIV.bordercol = SchemeAccent;
                        for (uint32_t j = 0; j < 128; j++) {
                            sdcard_readsector(freeze_slot_start_sector + i + j);
                            lcopy((uint32_t)sector_buffer, 0x40000U + (j << 9), SD_SECTOR_SIZE);
                        }
                        VICIV.bordercol = SchemeBorderBusy;
                        for (uint32_t j = 0; j < 128; j++) {
                            lcopy(0x40000U + (j << 9), (uint32_t)sector_buffer, SD_SECTOR_SIZE);
                            (void)sdcard_writesector(dest_freeze_slot_start_sector + i + j, 0);
                        }
                    }
                    VICIV.bordercol = SchemeBorder;

                    draw_freeze_menu(UpdateTop | UpdateProcess | UpdateThumb);
                } break;

                case KEY_F14: // F14 - restore CHARSET from FILE
                {
                    // clear screen first
                    predraw_freeze_menu();
                    // don't check, just put font into chargen
                    fix_chargen_area(ChargenNoCheck | ChargenFixMem);
                    // we need to redraw everything, because loading the ROM
                    // will mess things up (thumbnail for example)
                    last_thumb_frame = -1; // invalidate thumbnail
                    draw_freeze_menu(UpdateAll);
                } break;

                case KEY_HELP: // HELP MEGAINFO
                    start_freezer_tool("MEGAINFO.M65");
                    break;

                case 'R':
                case 'r': // switch CRT Emulation
                    key = freeze_io_peek(0x3054);
                    if (key & 0x20) {
                        freeze_io_poke(0x3054, key & 0xdf);
                        VICIV.ctrlc = VICIV.ctrlc & 0xdf;
                    } else {
                        freeze_io_poke(0x3054, key | 0x20);
                        VICIV.ctrlc = VICIV.ctrlc | 0x20;
                    }
                    draw_freeze_menu(UpdateTop);
                    break;
                case 'L':
                case 'l':
                    start_freezer_tool("ROMLOAD.M65");
                    break;
                case 'X':
                case 'x': // Poke finder
                case 'E':
                case 'e': // Enter POKEs
                case 'k':
                case 'K': // Sprite killer
                default:
                invalid_function:
                    // For invalid or unimplemented functions flash the border and screen
                    VICIV.bordercol = SchemeReject;
                    VICIV.screencol = SchemeReject;
                    usleep(150000L);
                    VICIV.bordercol = SchemeBorder;
                    VICIV.screencol = SchemeBackground;
                    break;
            }
        }
    }

    return 0;
}
