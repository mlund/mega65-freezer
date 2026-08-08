#include "colours.h"
#include "common.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

#include <mega65.h>
#include <stdio.h>
#include <string.h>

/* Arrays, not pointers to literals: draw_db_bar() and friends POKE into these
 * to build the display.  Writing through a string literal is undefined
 * behaviour, and the two share a long prefix so clang may merge them. */
unsigned char audio_menu[] = "         MEGA65 AUDIO MIXER MENU        "
                             "  (C) FLINDERS UNI, M.E.G.A. 2018-2024  "
                             " cccccccccccccccccccccccccccccccccccccc "
                             "        LFT RGT PH1 PH2 BTL BTR HDL HDR "
                             "        cccccccccccccccccccccccccccccccc"
                             "   SIDLb                                "
                             "   SIDRb                                "
                             " PHONE1b                                "
                             " PHONE2b                                "
                             "BTOOTHLb                                "
                             "BTOOTHRb                                "
                             "LINEINLb                                "
                             "LINEINRb                                "
                             "  DIGILb                                "
                             "  DIGIRb                                "
                             "  MIC0Lb                                "
                             "  MIC0Rb                                "
                             "  MIC1Lb                                "
                             "  MIC1Rb                                "
                             " OPL FMb                                "
                             " MASTERb                                "
                             " cccccccccccccccccccccccccccccccccccccc "
                             " T - TEST SOUND, CURSOR KEYS - NAVIGATE "
                             " +/- ADJUST VALUE,    0/* - FAST ADJUST "
                             " F3 - SIMPLE MODE,  M - TOGGLE MIC MUTE "
                             "\0";

unsigned char audio_menu_simple[] = "         MEGA65 AUDIO MIXER MENU        "
                                    "  (C) FLINDERS UNI, M.E.G.A. 2018-2024  "
                                    " cccccccccccccccccccccccccccccccccccccc "
                                    "                                        "
                                    "         LEFT OUTPUT CHANNEL:           "
                                    "        cccccccccccccccccccccccccccccccc"
                                    "    MASTERb                             "
                                    " L SID 3+4b                             "
                                    " R SID 1+2b                             "
                                    " LEFT DIGIb                             "
                                    "RIGHT DIGIb                             "
                                    "SFX OPL FMb                             "
                                    "                                        "
                                    "        RIGHT OUTPUT CHANNEL:           "
                                    "        cccccccccccccccccccccccccccccccc"
                                    "    MASTERb                             "
                                    " L SID 3+4b                             "
                                    " R SID 1+2b                             "
                                    " LEFT DIGIb                             "
                                    "RIGHT DIGIb                             "
                                    "SFX OPL FMb                             "
                                    " cccccccccccccccccccccccccccccccccccccc "
                                    " T - TEST SOUND, CURSOR KEYS - NAVIGATE "
                                    " +/- VOL, S - STEREO/MONO, W - SWAP L/R "
                                    " F3 - EXIT, M - MUTE, A - ADVANCED MODE "
                                    "\0";

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

void draw_advanced_mixer(void) {
    uint16_t offset;
    uint8_t colour;
    uint8_t c;
    uint8_t value;

    lpoke(COLOUR_RAM_ADDRESS + 3 * SCREEN_ROW_BYTES + 5, SchemeTextDim);
    lpoke(COLOUR_RAM_ADDRESS + 3 * SCREEN_ROW_BYTES + 7, SchemeTextDim);
    lpoke(COLOUR_RAM_ADDRESS + 3 * SCREEN_ROW_BYTES + 11, SchemeTextDim);
    lpoke(COLOUR_RAM_ADDRESS + 3 * SCREEN_ROW_BYTES + 13, SchemeTextDim);
    audio_menu[3 * 40 + 2] = nybl_to_screen(select_column);
    audio_menu[3 * 40 + 3] = nybl_to_screen(select_row);

    c = 0;
    do {

        // Work out address of where to draw the value
        offset = 8 + 5 * 40;              // Start of first value location
        offset += ((c & 0x1e) >> 1) * 40; // Low bits of number indicate Y position
        offset += (c >> 3) & 0x1e;        // High bits pick the column
        offset += (c & 1) + (c & 1);      // lowest bit picks LSB/MSB
        if (c & 0x10) {
            offset -= 2; // XXX Why do we need this fudge factor?
        }

        // And get the value to display
        value = audioxbar_getcoefficient(c);
        audio_menu[offset] = nybl_to_screen(value >> 4);
        audio_menu[offset + 1] = nybl_to_screen(value);

        // Now pick the colour to display
        // We want to make it easy to find values, so we should
        // have pairs of columns for odd and even rows, and a
        // highlight colour for the currently selected coefficient
        // (or just reverse video)

        colour = SchemeTextDim;
        if (((c & 0x1e) >> 1) == select_row) {
            colour = SchemeHighlight;
        }
        if ((c >> 5) == select_column) {
            if (colour == SchemeHighlight) {
                colour = SchemeText;
            } else {
                colour = SchemeHighlight;
            }
        }
        if (colour == SchemeText) {
            audio_menu[3 * 40 + 5] = nybl_to_screen(c >> 4);
            audio_menu[3 * 40 + 6] = nybl_to_screen(c);
        }

        lpoke(COLOUR_RAM_ADDRESS + (offset + offset + 1), colour);
        lpoke(COLOUR_RAM_ADDRESS + (offset + offset + 3), colour);

    } while (++c);

    // Update the coefficients in the audio_menu display, then
    // display it after, so that we have no flicker

    // Freezer can't use printf() etc, because C64 ROM has not started, so ZP will be a mess
    // (in fact, most of memory contains what the frozen program had. Only our freezer program
    // itself has been loaded to replace some of RAM).
    for (uint16_t i = 0; audio_menu[i]; i++) {
        if ((audio_menu[i] >= 'A') && (audio_menu[i] <= 'Z')) {
            POKE(SCREEN_ADDRESS + i * 2 + 0, audio_menu[i] - 0x40);
        } else if ((audio_menu[i] >= 'a') && (audio_menu[i] <= 'z')) {
            POKE(SCREEN_ADDRESS + i * 2 + 0, audio_menu[i] - 0x20);
        } else {
            POKE(SCREEN_ADDRESS + i * 2 + 0, audio_menu[i]);
        }
        POKE(SCREEN_ADDRESS + i * 2 + 1, 0);
    }
}

// clang-format off
// Decimal 0..79 as text, indexed by the dB value.
static const char* const DB_TEXT[80] = {
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
  "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
  "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
  "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
  "40", "41", "42", "43", "44", "45", "46", "47", "48", "49",
  "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
  "60", "61", "62", "63", "64", "65", "66", "67", "68", "69",
  "70", "71", "72", "73", "74", "75", "76", "77", "78", "79" };

static const uint16_t MINUS_DB_TABLE[256] = {
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

unsigned char db_index = 0;

void level_to_db_index(uint16_t level) {
    db_index = 0;
    while (level < MINUS_DB_TABLE[db_index]) {
        db_index++;
    }
}

unsigned char db_text[11];
void draw_db_bar(unsigned char line, uint16_t level) {
    uint16_t bar_addr = (uint16_t)audio_menu_simple + line * 40 + 11;
    uint16_t i;
    // Work out the approximate db_index value of the signal
    level_to_db_index(level);

    // Now draw the db_index bar.  We allow upto 20 chars wide
    // for the range 0 -- -79db = 1/4 char per dB.
    for (i = 0; i < 20; i++) {
        if (db_index >= 39) {
            POKE(bar_addr + i, 0x20);
        } else {
            // Filled bar
            if ((39 - db_index) > ((i * 2))) {
                POKE(bar_addr + i, 0xa0);
                // Empty cell
            } else if ((39 - db_index) < (i * 2)) {
                POKE(bar_addr + i, 0x20);
                // 1/2
            } else if ((39 - db_index) == ((i * 2) + 0)) {
                POKE(bar_addr + i, 117);
            }
        }
    }

    // And the annotation to the right
    bar_addr += 23;
    if (!db_index) {
        snprintf((char*)db_text, 10, "  0DB");
        for (i = 0; db_text[i]; i++) {
            POKE(bar_addr + i, db_text[i]);
        }
    } else {
        i = 0;
        if (db_index > 79) {
            db_index = 79;
        }
        if (db_index < 10) {
            POKE(bar_addr, ' ');
            bar_addr++;
        }
        POKE(bar_addr, '-');
        i++;
        for (; DB_TEXT[db_index][i - 1]; i++) {
            POKE(bar_addr + i, DB_TEXT[db_index][i - 1]);
        }
        POKE(bar_addr + i, 'D');
        i++;
        POKE(bar_addr + i, 'B');
        i++;
        for (; i < 5; i++) {
            POKE(bar_addr + i, ' ');
        }
    }
}

uint16_t first_coefficient, second_coefficient;

void set_amplifier(unsigned char left_right, uint16_t first_coefficient) {
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
    if (left_right == first_coefficient) {
        return;
    }
}

void change_db(unsigned char row, unsigned char change) {
    /* Every caller passes 0, 6 or select_row (masked to 0-11 in both event
     * loops), so the switch below always assigns c; the initialiser only
     * covers row values outside that range. */
    uint8_t c = 0;
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

    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    level_to_db_index(first_coefficient);
    if (change == 0) { // minus 1
        if (db_index < 39) {
            db_index++;
        }
    } else { // plus 1
        if (db_index) {
            db_index--;
        }
    }
    first_coefficient = MINUS_DB_TABLE[db_index];
    audioxbar_setcoefficient(c + 0, (uint8_t)(first_coefficient & 0xff));
    audioxbar_setcoefficient(c + 1, (uint8_t)(first_coefficient >> 8));
    // change LFT/RGT (audio jack) to the same value in simple mixer!
    audioxbar_setcoefficient(c - 0xc0, (uint8_t)(first_coefficient & 0xff));
    audioxbar_setcoefficient(c - 0xc0 + 1, (uint8_t)(first_coefficient >> 8));

    if (row == 0) {
        set_amplifier(0, first_coefficient);
    }
    if (row == 6) {
        set_amplifier(1, first_coefficient);
    }
}

void swap_coefficients(unsigned char first, unsigned char second) {
    first_coefficient = audioxbar_getcoefficient(first);
    first_coefficient |= audioxbar_getcoefficient(first + 1) << 8;
    second_coefficient = audioxbar_getcoefficient(second);
    second_coefficient |= audioxbar_getcoefficient(second + 1) << 8;
    audioxbar_setcoefficient(first, (uint8_t)(second_coefficient & 0xff));
    audioxbar_setcoefficient(second, (uint8_t)(first_coefficient & 0xff));
    audioxbar_setcoefficient(first + 1, (uint8_t)(second_coefficient >> 8));
    audioxbar_setcoefficient(second + 1, (uint8_t)(first_coefficient >> 8));
}

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

void stereo_toggle(void) {
    // very simple check if the values of HDL for the sids are the same
    first_coefficient = audioxbar_getcoefficient(0xc0);
    second_coefficient = audioxbar_getcoefficient(0xc2);
    if (first_coefficient == second_coefficient) {
        first_coefficient = MINUS_DB_TABLE[4];
        second_coefficient = MINUS_DB_TABLE[8];
    } else {
        first_coefficient = second_coefficient = MINUS_DB_TABLE[6];
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
            audioxbar_setcoefficient((uint8_t)(0x00 + i), (uint8_t)(first_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x01 + i), (uint8_t)(first_coefficient >> 8));
            // Right SID
            audioxbar_setcoefficient((uint8_t)(0x02 + i), (uint8_t)(second_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x03 + i), (uint8_t)(second_coefficient >> 8));
            // Left Digi
            audioxbar_setcoefficient((uint8_t)(0x10 + i), (uint8_t)(first_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x11 + i), (uint8_t)(first_coefficient >> 8));
            // Right Digi
            audioxbar_setcoefficient((uint8_t)(0x12 + i), (uint8_t)(second_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x13 + i), (uint8_t)(second_coefficient >> 8));
            // OPL SFX FM
            audioxbar_setcoefficient((uint8_t)(0x1c + i), (uint8_t)(first_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x1d + i), (uint8_t)(first_coefficient >> 8));
        } else {
            // Right side output

            // Left SID
            audioxbar_setcoefficient((uint8_t)(0x00 + i), (uint8_t)(second_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x01 + i), (uint8_t)(second_coefficient >> 8));
            // Right SID
            audioxbar_setcoefficient((uint8_t)(0x02 + i), (uint8_t)(first_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x03 + i), (uint8_t)(first_coefficient >> 8));
            // Left Digi
            audioxbar_setcoefficient((uint8_t)(0x10 + i), (uint8_t)(second_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x11 + i), (uint8_t)(second_coefficient >> 8));
            // Right Digi
            audioxbar_setcoefficient((uint8_t)(0x12 + i), (uint8_t)(first_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x13 + i), (uint8_t)(first_coefficient >> 8));
            // OPL SFX FM
            audioxbar_setcoefficient((uint8_t)(0x1c + i), (uint8_t)(first_coefficient & 0xff));
            audioxbar_setcoefficient((uint8_t)(0x1d + i), (uint8_t)(first_coefficient >> 8));
        }
    }
}

unsigned char db_bar_highlight[SCREEN_ROW_BYTES];
unsigned char db_bar_lowlight[SCREEN_ROW_BYTES];

void draw_simple_mixer(void) {
    // Update the volume bars and dB levels
    // display it after, so that we have no flicker
    uint8_t c;

    // Left output channel
    c = 0xde; // Master volume control
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(6, first_coefficient);
    c = 0xc0; // Left SIDs
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(7, first_coefficient);
    c = 0xc2; // Right SIDs
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(8, first_coefficient);
    c = 0xd0; // Left DIGI
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(9, first_coefficient);
    c = 0xd2; // Right DIGI
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(10, first_coefficient);
    c = 0xdc; // OPL2 / FM
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(11, first_coefficient);

    // Right output channel
    c = 0xfe; // Master volume control
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(15, first_coefficient);
    c = 0xe0; // Left SIDs
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(16, first_coefficient);
    c = 0xe2; // Right SIDs
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(17, first_coefficient);
    c = 0xf0; // Left DIGI
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(18, first_coefficient);
    c = 0xf2; // Right DIGI
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(19, first_coefficient);
    c = 0xfc; // OPL2 / FM
    first_coefficient = audioxbar_getcoefficient(c);
    first_coefficient |= audioxbar_getcoefficient(c + 1) << 8;
    draw_db_bar(20, first_coefficient);

    // Freezer can't use printf() etc, because C64 ROM has not started, so ZP will be a mess
    // (in fact, most of memory contains what the frozen program had. Only our freezer program
    // itself has been loaded to replace some of RAM).
    for (uint16_t i = 0; audio_menu_simple[i]; i++) {
        if ((audio_menu_simple[i] >= '@') && (audio_menu_simple[i] <= 'Z')) {
            POKE(SCREEN_ADDRESS + i * 2 + 0, audio_menu_simple[i] - 0x40);
        } else if ((audio_menu_simple[i] >= 'b') && (audio_menu_simple[i] <= 'c')) {
            POKE(SCREEN_ADDRESS + i * 2 + 0, audio_menu_simple[i] - 0x20);
        } else {
            POKE(SCREEN_ADDRESS + i * 2 + 0, audio_menu_simple[i]);
        }
        POKE(SCREEN_ADDRESS + i * 2 + 1, 0);
    }

    // Work out the line to highlight

    select_column = 6 + select_row;
    if (select_row >= 6) {
        select_column += 3;
    }

    for (uint16_t i = 6; i < 21; i++) {
        if (i == select_column) {
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

unsigned char frames;
unsigned char note;
unsigned char sid_num;
uint16_t sid_addr;
static const uint16_t NOTES[5] = {5001, 5613, 4455, 2227, 3338};

void test_audio(unsigned char advanced_view) {
    /*
      Play notes and samples through 4 SIDs and left/right digi
    */

    // Reset all sids
    lfill(0xffd3400, 0, 0x100);

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

        sid_addr = 0xd400 + (0x20 * sid_num);

        // Play note
        POKE(sid_addr + 0, NOTES[note] & 0xff);
        POKE(sid_addr + 1, NOTES[note] >> 8);
        POKE(sid_addr + 4, 0x10);
        POKE(sid_addr + 5, 0x0c);
        POKE(sid_addr + 6, 0x00);
        POKE(sid_addr + 4, 0x11);

        if (advanced_view) {
            // Highlight the appropriate part of the screen
            for (uint16_t i = 5 * SCREEN_ROW_BYTES; i < 7 * SCREEN_ROW_BYTES; i += 2) {
                lpoke(0xff80001L + i, lpeek(0xff80001L + i) & 0x0f);
            }
            switch (sid_num) {
                case 0:
                    for (uint16_t i = 0; i < SCREEN_ROW_BYTES; i += 2) {
                        lpoke(0xff80001L + 6 * SCREEN_ROW_BYTES + i,
                            lpeek(0xff80001L + 6 * SCREEN_ROW_BYTES + i) | 0x20);
                    }
                    break;
                case 1:
                    for (uint16_t i = 0; i < SCREEN_ROW_BYTES; i += 2) {
                        lpoke(0xff80001L + 6 * SCREEN_ROW_BYTES + i,
                            lpeek(0xff80001L + 6 * SCREEN_ROW_BYTES + i) | 0x60);
                    }
                    break;
                case 2:
                    for (uint16_t i = 0; i < SCREEN_ROW_BYTES; i += 2) {
                        lpoke(0xff80001L + 5 * SCREEN_ROW_BYTES + i,
                            lpeek(0xff80001L + 5 * SCREEN_ROW_BYTES + i) | 0x20);
                    }
                    break;
                case 3:
                    for (uint16_t i = 0; i < SCREEN_ROW_BYTES; i += 2) {
                        lpoke(0xff80001L + 5 * SCREEN_ROW_BYTES + i,
                            lpeek(0xff80001L + 5 * SCREEN_ROW_BYTES + i) | 0x60);
                    }
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
            lpoke(0xff80001L + i, lpeek(0xff80001L + i) & 0x0f);
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
    lfill(0xffd3400, 0, 0x80);
}

unsigned char key;

void do_advanced_mixer(void) {
    uint8_t value;
    uint8_t coefficient;

    select_row = 0;
    select_column = 0;

    // reset colour ram
    clear_colour_ram();

    draw_advanced_mixer();

    // clear keybuffer
    while ((key = ASCIIKEY)) {
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
                    // reset colour ram
                    clear_colour_ram();
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

void do_audio_mixer(void) {
    uint8_t simple_row;

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
