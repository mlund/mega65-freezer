#include "colours.h"
#include "common.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

#include <mega65.h>

/* audiomix/menu.cpp: the fixed labels, converted to screen codes when it was
 * compiled.  Only the labels are fragments -- the coefficient matrix, the dB
 * bars and the row-3 readouts below are a live hex/level display, so they
 * stay runtime-drawn. */
extern const uint8_t* audiomix_banner_stream(void);
extern const uint8_t* audiomix_advanced_fixed_stream(void);
extern const uint8_t* audiomix_simple_fixed_stream(void);

void audioxbar_setcoefficient(uint8_t n, uint8_t value) {
    // Select the coefficient
    AUDIOMIX_REGSEL = n;

    // Now wait at least 16 cycles for it to settle
    VICIV.bordercol = VICIV.bordercol;
    VICIV.bordercol = VICIV.bordercol;
    VICIV.bordercol = VICIV.bordercol;

    AUDIOMIX_REGDATA = value;
}

uint8_t audioxbar_getcoefficient(uint8_t n) {
    // Select the coefficient
    AUDIOMIX_REGSEL = n;

    // Now wait at least 16 cycles for it to settle
    VICIV.bordercol = VICIV.bordercol;
    VICIV.bordercol = VICIV.bordercol;
    VICIV.bordercol = VICIV.bordercol;

    return AUDIOMIX_REGDATA;
}

/* The cursor position, which must survive from one call to the next: the
 * event loops in do_advanced_mixer() and do_audio_mixer() redraw after every
 * keypress rather than looping inside a single call. */
static uint8_t select_row, select_column;
static uint8_t mute_save[8];

/* Everything that does not change between keypresses.  Its own entry point,
 * called once per view entry, so the per-keypress redraw below only ever
 * touches the live matrix / bars / readouts.  draw_rule()/blank_screen() are
 * shared with FREEZER's menu (src/screen.c), which has the same shape. */
void draw_advanced_fixed(void) {
    clear_colour_ram();
    blank_screen();
    draw_fragments(audiomix_banner_stream());
    draw_rule(SCREEN_CELL(1, 2), 38);
    draw_fragments(audiomix_advanced_fixed_stream());
    draw_rule(SCREEN_CELL(8, 4), 32);
    draw_rule(SCREEN_CELL(1, 21), 38);
}

// Draws the simple view's title, headings and row labels.
void draw_simple_fixed(void) {
    clear_colour_ram();
    blank_screen();
    draw_fragments(audiomix_banner_stream());
    draw_rule(SCREEN_CELL(1, 2), 38);
    draw_fragments(audiomix_simple_fixed_stream());
    draw_rule(SCREEN_CELL(8, 5), 32);
    draw_rule(SCREEN_CELL(8, 14), 32);
    draw_rule(SCREEN_CELL(1, 21), 38);
}

/* The coefficient matrix: row 3 columns 0-1 is the cursor position, columns
 * 4-5 the currently selected coefficient's own value -- both live, redrawn
 * here every keypress.  Row/column arithmetic is unchanged from the original:
 * bit 4 of the coefficient index feeds both the row selector (as its top bit)
 * and the column selector (as its bottom bit), which is what the `-2`
 * adjustment compensates for when it is set. */
void draw_advanced_mixer(void) {
    uint8_t c;
    char selected[2];
    char readout[2];

    readout[0] = (char)nybl_to_screen(select_column);
    readout[1] = (char)nybl_to_screen(select_row);
    draw_text(SCREEN_CELL(2, 3), SchemeAccent, readout, 2);

    c = 0;
    do {
        uint8_t row_index = (c & 0x1e) >> 1;
        uint8_t y = (uint8_t)(5 + row_index);
        uint8_t x = (uint8_t)(8 + ((c >> 3) & 0x1e) + 2 * (c & 1));
        uint8_t value;
        uint8_t colour;
        char digits[2];

        if (c & 0x10) {
            x -= 2;
        }

        value = audioxbar_getcoefficient(c);
        digits[0] = (char)nybl_to_screen(value >> 4);
        digits[1] = (char)nybl_to_screen(value);

        // Colour: dim baseline, highlight on a matching row or column, and
        // the brighter label colour where both match -- the actively
        // selected coefficient.
        colour = SchemeValue;
        if (row_index == select_row) {
            colour = SchemeHighlight;
        }
        if ((c >> 5) == select_column) {
            colour = (colour == SchemeHighlight) ? SchemeText : SchemeHighlight;
        }
        if (colour == SchemeText) {
            selected[0] = (char)nybl_to_screen(c >> 4);
            selected[1] = (char)nybl_to_screen(c);
        }

        draw_text(SCREEN_CELL(x, y), colour, digits, 2);
    } while (++c);

    // row_index covers 0-15 and select_column is masked to 0-7, so some c
    // always matches both axes and sets `selected` above.
    draw_text(SCREEN_CELL(5, 3), SchemeAccent, selected, 2);
}

static const uint16_t MINUS_DB_TABLE[256] = {
    // clang-format off
  65535L, 52026L, 41303L, 32789L, 26031, 20665, 16406, 13024,
  10339, 8208, 6516, 5173, 4107, 3260, 2588, 2054,
  1631, 1295, 1028, 816, 648, 514, 408, 324,
  257, 204, 162, 128, 102, 81, 64, 51,
  40, 32, 25, 20, 16, 12, 10, 8,
  6, 5, 4, 3, 2, 2, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 0 };
// clang-format on

// The dB step (0 = 0dB, 79 = -79dB) whose table entry `level` still reaches.
static uint8_t level_to_db_index(uint16_t level) {
    uint8_t index = 0;
    while (level < MINUS_DB_TABLE[index]) {
        index++;
    }
    return index;
}

/* Bar plus dB annotation for one row of the simple view.  The bar is filled
 * and blanked with raw screen codes via lfill_skip() -- not draw_text(),
 * whose ASCII case-fold would corrupt the half-block glyph (0x75 is 'u').
 * Colour for the bar is set separately, by the whole-row lcopy in
 * draw_simple_mixer(); only the annotation's colour is set here. */
void draw_db_bar(unsigned char line, uint16_t level) {
    uint8_t db_index = level_to_db_index(level);
    char annotation[5];
    char* at = annotation;

    if (db_index >= 39) {
        lfill_skip(SCREEN_ADDRESS + SCREEN_CELL(11, line), ' ', 20, SCREEN_CELL_BYTES);
    } else {
        // Up to 20 chars wide for the range 0..-79dB = 1/4 char per dB.  diff
        // is 1..39 here (db_index < 39), so full_count is always 1..20.
        uint8_t diff = (uint8_t)(39 - db_index);
        uint8_t full_count = (uint8_t)((diff + 1) / 2);
        uint8_t drawn;
        lfill_skip(SCREEN_ADDRESS + SCREEN_CELL(11, line), 0xa0, full_count, SCREEN_CELL_BYTES);
        drawn = full_count;
        if (!(diff & 1) && drawn < 20) {
            lfill_skip(SCREEN_ADDRESS + SCREEN_CELL((uint8_t)(11 + drawn), line),
                117,
                1,
                SCREEN_CELL_BYTES);
            drawn++;
        }
        if (drawn < 20) {
            lfill_skip(SCREEN_ADDRESS + SCREEN_CELL((uint8_t)(11 + drawn), line),
                ' ',
                (uint8_t)(20 - drawn),
                SCREEN_CELL_BYTES);
        }
    }

    if (!db_index) {
        at = append_str(at, "  0DB");
    } else {
        uint8_t index = (db_index > 79) ? 79 : db_index;
        if (index < 10) {
            *at++ = ' ';
        }
        *at++ = '-';
        at = append_dec(at, index);
        at = append_str(at, "DB");
    }
    draw_text(SCREEN_CELL(34, line), SchemeValue, annotation, (uint8_t)(at - annotation));
}

/* Both bytes of one coefficient, low then high -- the shape every caller
 * below needs before it can compare or store a 16-bit level. */
static uint16_t read_coefficient_pair(uint8_t c) {
    uint16_t value = audioxbar_getcoefficient(c);
    value |= (uint16_t)audioxbar_getcoefficient(c + 1) << 8;
    return value;
}

void set_amplifier(unsigned char left_right, uint16_t coefficient) {
    /*
      Map 16-bit unsigned volume level to amplifier level.
      This is not super simple, as amplifier value $00 = +24dB,
      which is not a good idea to go that high.
      $20 is safe enough on the MEGA65 R3, but $28 is about the
      limit on the MEGAphone without causing power rail sagging
      on maximum volume.  $28 should thus not be in the "red"
      zone of the mixer.

      $FF is effectively mute on the amplifier.

      So $0000 = $FF and $FFFF = $20
      So a linear mapping between those should be fine.

    */

    // This does not work, disabled!

    // avoid compiler warning because of disabled code
    if (left_right == coefficient) {
        return;
    }
}

// Steps one channel's HDMI coefficient, and its jack twin, one dB up
// (change != 0) or down.
void change_db(unsigned char row, unsigned char change) {
    /* Every caller passes 0, 6 or select_row (masked to 0-11 in both event
     * loops), so the switch below always assigns c; the initialiser only
     * covers row values outside that range. */
    uint8_t c = 0;
    uint16_t coefficient;
    uint8_t db_index;
    // clang-format off
  // this is all HDL/HDR (HDMI) channels
  switch (row) {
  case  0: c = 0xde; break;
  case  1: c = 0xc0; break;
  case  2: c = 0xc2; break;
  case  3: c = 0xd0; break;
  case  4: c = 0xd2; break;
  case  5: c = 0xdc; break;
  case  6: c = 0xfe; break;
  case  7: c = 0xe0; break;
  case  8: c = 0xe2; break;
  case  9: c = 0xf0; break;
  case 10: c = 0xf2; break;
  case 11: c = 0xfc; break;
      default:
          break;
  }
    // clang-format on

    coefficient = read_coefficient_pair(c);
    db_index = level_to_db_index(coefficient);
    if (change == 0) { // minus 1
        if (db_index < 39) {
            db_index++;
        }
    } else { // plus 1
        if (db_index) {
            db_index--;
        }
    }
    coefficient = MINUS_DB_TABLE[db_index];
    audioxbar_setcoefficient(c + 0, (uint8_t)(coefficient & 0xff));
    audioxbar_setcoefficient(c + 1, (uint8_t)(coefficient >> 8));
    // change LFT/RGT (audio jack) to the same value in simple mixer!
    audioxbar_setcoefficient(c - 0xc0, (uint8_t)(coefficient & 0xff));
    audioxbar_setcoefficient(c - 0xc0 + 1, (uint8_t)(coefficient >> 8));

    if (row == 0) {
        set_amplifier(0, coefficient);
    }
    if (row == 6) {
        set_amplifier(1, coefficient);
    }
}

// Exchanges two coefficients' 16-bit values.
void swap_coefficients(unsigned char first, unsigned char second) {
    uint16_t first_value = read_coefficient_pair(first);
    uint16_t second_value = read_coefficient_pair(second);
    audioxbar_setcoefficient(first, (uint8_t)(second_value & 0xff));
    audioxbar_setcoefficient(second, (uint8_t)(first_value & 0xff));
    audioxbar_setcoefficient(first + 1, (uint8_t)(second_value >> 8));
    audioxbar_setcoefficient(second + 1, (uint8_t)(first_value >> 8));
}

// Swaps every left/right coefficient pair, both HDMI and jack outputs.
void stereo_swap(void) {
    // Swap left and right sides
    // HDMI
    swap_coefficients(0xc0, 0xe0);
    swap_coefficients(0xc2, 0xe2);
    swap_coefficients(0xd0, 0xf0);
    swap_coefficients(0xd2, 0xf2);
    swap_coefficients(0xdc, 0xfc);
    // JACK
    swap_coefficients(0x00, 0x20);
    swap_coefficients(0x02, 0x22);
    swap_coefficients(0x10, 0x30);
    swap_coefficients(0x12, 0x32);
    swap_coefficients(0x1c, 0x3c);
}

// Toggles between a 12dB left/right split and equal, centred SIDs/digi/FM.
void stereo_toggle(void) {
    // very simple check if the values of HDL for the sids are the same
    uint16_t first_value = audioxbar_getcoefficient(0xc0);
    uint16_t second_value = audioxbar_getcoefficient(0xc2);
    if (first_value == second_value) {
        first_value = MINUS_DB_TABLE[4];
        second_value = MINUS_DB_TABLE[8];
    } else {
        first_value = second_value = MINUS_DB_TABLE[6];
    }

    // Make stereo with 12dB difference between left and right
    for (uint16_t j = 0; j < 4; j++) {
        uint16_t i = j * 0x20;
        if (j > 1) { // HDL/HDR
            i += 0x80;
        }
        if (!(i & 0x20)) {
            // Left side output

            // Left SID
            audioxbar_setcoefficient((uint8_t)(0x00 + i), (uint8_t)(first_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x01 + i), (uint8_t)(first_value >> 8));
            // Right SID
            audioxbar_setcoefficient((uint8_t)(0x02 + i), (uint8_t)(second_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x03 + i), (uint8_t)(second_value >> 8));
            // Left Digi
            audioxbar_setcoefficient((uint8_t)(0x10 + i), (uint8_t)(first_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x11 + i), (uint8_t)(first_value >> 8));
            // Right Digi
            audioxbar_setcoefficient((uint8_t)(0x12 + i), (uint8_t)(second_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x13 + i), (uint8_t)(second_value >> 8));
            // OPL SFX FM
            audioxbar_setcoefficient((uint8_t)(0x1c + i), (uint8_t)(first_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x1d + i), (uint8_t)(first_value >> 8));
        } else {
            // Right side output

            // Left SID
            audioxbar_setcoefficient((uint8_t)(0x00 + i), (uint8_t)(second_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x01 + i), (uint8_t)(second_value >> 8));
            // Right SID
            audioxbar_setcoefficient((uint8_t)(0x02 + i), (uint8_t)(first_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x03 + i), (uint8_t)(first_value >> 8));
            // Left Digi
            audioxbar_setcoefficient((uint8_t)(0x10 + i), (uint8_t)(second_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x11 + i), (uint8_t)(second_value >> 8));
            // Right Digi
            audioxbar_setcoefficient((uint8_t)(0x12 + i), (uint8_t)(first_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x13 + i), (uint8_t)(first_value >> 8));
            // OPL SFX FM
            audioxbar_setcoefficient((uint8_t)(0x1c + i), (uint8_t)(first_value & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x1d + i), (uint8_t)(first_value >> 8));
        }
    }
}

static unsigned char db_bar_highlight[SCREEN_ROW_BYTES];
static unsigned char db_bar_lowlight[SCREEN_ROW_BYTES];

// Redraws all twelve dB bars and recolours the selected row.
void draw_simple_mixer(void) {
    uint8_t highlight_row;

    // Left output channel
    draw_db_bar(6, read_coefficient_pair(0xde));  // Master volume control
    draw_db_bar(7, read_coefficient_pair(0xc0));  // Left SIDs
    draw_db_bar(8, read_coefficient_pair(0xc2));  // Right SIDs
    draw_db_bar(9, read_coefficient_pair(0xd0));  // Left DIGI
    draw_db_bar(10, read_coefficient_pair(0xd2)); // Right DIGI
    draw_db_bar(11, read_coefficient_pair(0xdc)); // OPL2 / FM

    // Right output channel
    draw_db_bar(15, read_coefficient_pair(0xfe)); // Master volume control
    draw_db_bar(16, read_coefficient_pair(0xe0)); // Left SIDs
    draw_db_bar(17, read_coefficient_pair(0xe2)); // Right SIDs
    draw_db_bar(18, read_coefficient_pair(0xf0)); // Left DIGI
    draw_db_bar(19, read_coefficient_pair(0xf2)); // Right DIGI
    draw_db_bar(20, read_coefficient_pair(0xfc)); // OPL2 / FM

    // Work out the line to highlight
    highlight_row = 6 + select_row;
    if (select_row >= 6) {
        highlight_row += 3;
    }

    for (uint16_t i = 6; i < 21; i++) {
        if (i == highlight_row) {
            // Highligh colouring
            lcopy((long)db_bar_highlight,
                COLOUR_RAM_ADDRESS + i * SCREEN_ROW_BYTES,
                SCREEN_ROW_BYTES);
        } else {
            // Normal colouring
            if (i < 12 || i > 14) {
                lcopy((long)db_bar_lowlight,
                    COLOUR_RAM_ADDRESS + i * SCREEN_ROW_BYTES,
                    SCREEN_ROW_BYTES);
            }
        }
    }
}

/* $D400, reached via its 28-bit flat alias so lfill() can zero all four SID
 * chips' register files (0x100 bytes) in one DMA job rather than four
 * per-chip loops. */
static constexpr Addr28 SID_ALL_28BIT = 0xffd3400;

static volatile struct __sid* const SIDS[4] = {&SID1, &SID2, &SID3, &SID4};

static const uint16_t NOTES[5] = {5001, 5613, 4455, 2227, 3338};

/* OR mask into every colour-RAM attribute byte of one row, in test_audio()'s
 * advanced-view highlight -- the four SIDs' rows/masks differ, the walk does
 * not. */
static void highlight_sid_row(uint8_t row, uint8_t mask) {
    Addr28 row_colour =
        COLOUR_RAM_ADDRESS + (uint16_t)(row * SCREEN_ROW_BYTES + (SCREEN_CELL_BYTES - 1));
    for (uint16_t i = 0; i < SCREEN_ROW_BYTES; i += 2) {
        Addr28 cell_addr = row_colour + i;
        lpoke(cell_addr, lpeek(cell_addr) | mask);
    }
}

void test_audio(unsigned char advanced_view) {
    /*
      Play notes and samples through 4 SIDs and left/right digi
    */
    unsigned char frames;
    unsigned char note;
    unsigned char sid_num = 0;

    // Reset all sids
    lfill(SID_ALL_28BIT, 0, 0x100);

    // Full volume on all SIDs
    SID1.amp = 0x0f;
    SID2.amp = 0x0f;
    SID3.amp = 0x0f;
    SID4.amp = 0x0f;

    for (note = 0; note < 5; note++) {
        // clang-format off
      switch(note) {
      case 0: sid_num = 0; break;
      case 1: sid_num = 2; break;
      case 2: sid_num = 1; break;
      case 3: sid_num = 3; break;
      case 4: sid_num = 0; break;
          default:
              break;
      }
        // clang-format on

        // Play note
        SIDS[sid_num]->v1.freq = NOTES[note];
        SIDS[sid_num]->v1.ctrl = 0x10;
        SIDS[sid_num]->v1.ad = 0x0c;
        SIDS[sid_num]->v1.sr = 0x00;
        SIDS[sid_num]->v1.ctrl = 0x11;

        if (advanced_view) {
            // Highlight the appropriate part of the screen
            for (uint16_t i = 5 * SCREEN_ROW_BYTES; i < 7 * SCREEN_ROW_BYTES; i += 2) {
                Addr28 cell_addr = COLOUR_RAM_ADDRESS + (uint16_t)(i + (SCREEN_CELL_BYTES - 1));
                lpoke(cell_addr, lpeek(cell_addr) & 0x0f);
            }
            switch (sid_num) {
                case 0:
                    highlight_sid_row(6, 0x20);
                    break;
                case 1:
                    highlight_sid_row(6, 0x60);
                    break;
                case 2:
                    highlight_sid_row(5, 0x20);
                    break;
                case 3:
                    highlight_sid_row(5, 0x60);
                    break;
                default:
                    break;
            }
        } else {
            switch (sid_num) {
                case 0:
                case 1:
                    lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 7 * SCREEN_ROW_BYTES, 80);
                    lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 16 * SCREEN_ROW_BYTES, 80);
                    lcopy((long)db_bar_highlight, COLOUR_RAM_ADDRESS + 8 * SCREEN_ROW_BYTES, 80);
                    lcopy((long)db_bar_highlight, COLOUR_RAM_ADDRESS + 17 * SCREEN_ROW_BYTES, 80);
                    break;
                case 2:
                case 3:
                    lcopy((long)db_bar_highlight, COLOUR_RAM_ADDRESS + 7 * SCREEN_ROW_BYTES, 80);
                    lcopy((long)db_bar_highlight, COLOUR_RAM_ADDRESS + 16 * SCREEN_ROW_BYTES, 80);
                    lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 8 * SCREEN_ROW_BYTES, 80);
                    lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 17 * SCREEN_ROW_BYTES, 80);
                    break;
                default:
                    break;
            }
        }

        // Wait 1/2 second before next note
        // (==25 frames)
        /*
           So the trick here, is that we need to decide if we are doing 4-SID mode,
           where all SIDs are 1/2 volume (gain can of course be increased to compensate),
           or whether we allow the primary pair of SIDs to be louder.
           We have to write to 4-SID registers at least every couple of frames to keep them active
        */
        for (frames = 0; frames < 35; frames++) {
            // Make sure all 4 SIDs remain active
            // by proding while waiting
            while (VICIV.rasterline != 0x80) {
                SID2.amp = 0x0f;
                SID4.amp = 0x0f;
            }

            while (VICIV.rasterline == 0x80) {
            }
        }
    }

    // Clear highlight
    if (advanced_view) {
        for (uint16_t i = 5 * SCREEN_ROW_BYTES; i < 7 * SCREEN_ROW_BYTES; i += 2) {
            Addr28 cell_addr = COLOUR_RAM_ADDRESS + (uint16_t)(i + (SCREEN_CELL_BYTES - 1));
            lpoke(cell_addr, lpeek(cell_addr) & 0x0f);
        }
    } else {
        lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 9 * SCREEN_ROW_BYTES, 80);
        lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 17 * SCREEN_ROW_BYTES, 80);
        lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 7 * SCREEN_ROW_BYTES, 80);
        lcopy((long)db_bar_lowlight, COLOUR_RAM_ADDRESS + 15 * SCREEN_ROW_BYTES, 80);
    }
    // Silence SIDs gradually to avoid pops
    /*
    for (frames = 15; frames < 16; frames--) {
      while (VICIV.rasterline != 0x80); // wait for raster
      SID1.amp = frames;
      SID2.amp = frames;
      SID3.amp = frames;
      SID4.amp = frames;
    }
    */
    while (VICIV.rasterline != 0x80) {
        ;
    }
    SID1.amp = 0x0;
    SID2.amp = 0x0;
    SID3.amp = 0x0;
    SID4.amp = 0x0;

    // Reset all sids
    lfill(SID_ALL_28BIT, 0, 0x80);
}

// The advanced view's event loop: cursor keys move the matrix selection,
// +/-/0/* adjust the selected coefficient, T tests, M toggles mic mute.
void do_advanced_mixer(void) {
    uint8_t value;
    uint8_t coefficient;
    uint8_t key;

    select_row = 0;
    select_column = 0;

    draw_advanced_fixed();
    draw_advanced_mixer();

    // clear keybuffer
    while (ASCIIKEY) {
        ASCIIKEY = 0;
    }

    while (1) {
        key = ASCIIKEY;
        if (key) {
            // Flush char from input buffer
            ASCIIKEY = 0;

            // Get coefficient number ready
            coefficient = (uint8_t)((select_column << 5) + (select_row << 1) + 1);
            value = audioxbar_getcoefficient(coefficient);

            // Process char
            switch (key) {
                case KEY_RUN_STOP:
                case KEY_F3: // RUN/STOP or F3 to exit
                    return;
                case KEY_CURSOR_DOWN:
                    select_row++;
                    select_row &= 0x0f;
                    break;
                case KEY_CURSOR_RIGHT:
                    select_column++;
                    select_column &= 0x7;
                    break;
                case KEY_CURSOR_UP:
                    select_row--;
                    select_row &= 0x0f;
                    break;
                case KEY_CURSOR_LEFT:
                    select_column--;
                    select_column &= 0x7;
                    break;
                case '+':
                    value++;
                    audioxbar_setcoefficient(coefficient - 1, value);
                    audioxbar_setcoefficient(coefficient, value);
                    break;
                case '0':
                    value += 0x10;
                    audioxbar_setcoefficient(coefficient - 1, value);
                    audioxbar_setcoefficient(coefficient, value);
                    break;
                case '-':
                    value--;
                    audioxbar_setcoefficient(coefficient - 1, value);
                    audioxbar_setcoefficient(coefficient, value);
                    break;
                case '*':
                    value -= 0x10;
                    audioxbar_setcoefficient(coefficient - 1, value);
                    audioxbar_setcoefficient(coefficient, value);
                    break;
                case 't':
                case 'T':
                    test_audio(1);
                    break;
                case 'm':
                case 'M':
                    if (audioxbar_getcoefficient(0x14)) {
                        for (uint16_t i = 0x00; i < 0x100; i += 0x20) {
                            audioxbar_setcoefficient((uint8_t)(i + 0x14), 0);
                            audioxbar_setcoefficient((uint8_t)(i + 0x15), 0);
                            audioxbar_setcoefficient((uint8_t)(i + 0x16), 0);
                            audioxbar_setcoefficient((uint8_t)(i + 0x17), 0);
                        }
                    } else {
                        for (uint16_t i = 0x00; i < 0x100; i += 0x20) {
                            audioxbar_setcoefficient((uint8_t)(i + 0x14), 0x30);
                            audioxbar_setcoefficient((uint8_t)(i + 0x15), 0x30);
                            audioxbar_setcoefficient((uint8_t)(i + 0x16), 0x30);
                            audioxbar_setcoefficient((uint8_t)(i + 0x17), 0x30);
                        }
                    }
                    break;
                default:
                    // For invalid or unimplemented functions flash the border and screen
                    VICIV.bordercol = SchemeReject;
                    VICIV.screencol = SchemeReject;
                    usleep(150000L);
                    VICIV.bordercol = SchemeBorder;
                    VICIV.screencol = SchemeBackground;
                    key = 0;
                    break;
            }
            if (key) {
                draw_advanced_mixer();
            }
        }
    }
}

// The simple view's event loop, and AUDIOMIX's entry point: cursor keys pick
// a row and adjust it, A enters the advanced view, T tests, W/S/M swap,
// toggle stereo and mute.
void do_audio_mixer(void) {
    uint8_t simple_row;
    uint8_t key;

    select_row = 0;

    for (uint16_t i = 0; i < 80; i += 2) {
        if (i >= 22 && i < 46) {
            db_bar_highlight[i + 1] = SchemeMeterLow;
            db_bar_lowlight[i + 1] = SchemeMeterLowDim;
        } else if (i >= 46 && i < 54) {
            db_bar_highlight[i + 1] = SchemeMeterMid;
            db_bar_lowlight[i + 1] = SchemeMeterMidDim;
        } else if (i >= 54 && i < 68) {
            db_bar_highlight[i + 1] = SchemeMeterHigh;
            db_bar_lowlight[i + 1] = SchemeMeterHighDim;
        } else {
            db_bar_highlight[i + 0] = 0;
            db_bar_lowlight[i + 0] = 0;
            db_bar_highlight[i + 1] = SchemeText;
            db_bar_lowlight[i + 1] = SchemeTextDim;
        }
    }

    draw_simple_fixed();
    draw_simple_mixer();

    // clear keybuffer
    while ((key = ASCIIKEY)) {
        ASCIIKEY = key;
    }

    while (1) {
        key = ASCIIKEY;
        if (key) {
            // Flush char from input buffer
            ASCIIKEY = 0;

            switch (key) {
                case KEY_RUN_STOP:
                case KEY_F3: // RUN/STOP / F3 = Exit
                    return;
                case 'A':
                case 'a': // Advanced mode
                    simple_row = select_row;
                    do_advanced_mixer();
                    select_row = simple_row;
                    draw_simple_fixed();
                    break;
                case '+':
                    change_db(0, 1); // master left
                    change_db(6, 1); // master right
                    break;
                case '-':
                    change_db(0, 0); // master left
                    change_db(6, 0); // master right
                    break;
                case KEY_CURSOR_RIGHT: // Right = + 1 to DB of signal
                    change_db(select_row, 1);
                    break;
                case KEY_CURSOR_LEFT: // Left = -1 to DB of signal
                    change_db(select_row, 0);
                    break;
                case KEY_CURSOR_DOWN:
                    select_row++;
                    if (select_row >= 12) {
                        select_row = 0;
                    }
                    break;
                case KEY_CURSOR_UP:
                    select_row--;
                    if (select_row >= 12) {
                        select_row = 11;
                    }
                    break;
                case 't':
                case 'T':
                    test_audio(0); // simple view highlighting
                    break;
                case 'w':
                case 'W':
                    // Switch coefficients for left and right channels
                    stereo_swap();
                    break;
                case 's':
                case 'S':
                    stereo_toggle();
                    break;
                case 'm':
                case 'M': // Mute
                    if (audioxbar_getcoefficient(0xfe)) {
                        mute_save[0] = audioxbar_getcoefficient(0x1e);
                        mute_save[1] = audioxbar_getcoefficient(0x1f);
                        mute_save[2] = audioxbar_getcoefficient(0x3e);
                        mute_save[3] = audioxbar_getcoefficient(0x3f);
                        mute_save[4] = audioxbar_getcoefficient(0xde);
                        mute_save[5] = audioxbar_getcoefficient(0xdf);
                        mute_save[6] = audioxbar_getcoefficient(0xfe);
                        mute_save[7] = audioxbar_getcoefficient(0xff);
                        audioxbar_setcoefficient(0x1e, 0);
                        audioxbar_setcoefficient(0x1f, 0);
                        audioxbar_setcoefficient(0x3e, 0);
                        audioxbar_setcoefficient(0x3f, 0);
                        audioxbar_setcoefficient(0xde, 0);
                        audioxbar_setcoefficient(0xdf, 0);
                        audioxbar_setcoefficient(0xfe, 0);
                        audioxbar_setcoefficient(0xff, 0);
                    } else {
                        audioxbar_setcoefficient(0x1e, mute_save[0]);
                        audioxbar_setcoefficient(0x1f, mute_save[1]);
                        audioxbar_setcoefficient(0x3e, mute_save[2]);
                        audioxbar_setcoefficient(0x3f, mute_save[3]);
                        audioxbar_setcoefficient(0xde, mute_save[4]);
                        audioxbar_setcoefficient(0xdf, mute_save[5]);
                        audioxbar_setcoefficient(0xfe, mute_save[6]);
                        audioxbar_setcoefficient(0xff, mute_save[7]);
                    }
                    break;
                default:
                    // For invalid or unimplemented functions flash the border and screen
                    VICIV.bordercol = SchemeReject;
                    VICIV.screencol = SchemeReject;
                    usleep(150000L);
                    VICIV.bordercol = SchemeBorder;
                    VICIV.screencol = SchemeBackground;
                    key = 0;
                    break;
            }
            // only draw menu if there was a keypress to handle
            if (key) {
                draw_simple_mixer();
            }
        }
    }
}
