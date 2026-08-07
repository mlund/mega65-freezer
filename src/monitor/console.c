/* MEGA65 only: writes the VIC-IV screen directly. */

#include "console.h"

#include "colours.h"
#include "common.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"

#include <mega65.h>

extern unsigned char* charset;

char* footer_messages[FooterMax + 1] = {
    /* Exactly 80 columns: display_footer() copies the whole row unconditionally. */
    "(A)SM (B)IT (C)MP (D)ASM (F)ILL (H)UNT (M)EM (R)EG (S)ET (T)RA E(X)IT SETREG(;) ",
    "A FATAL ERROR HAS OCCURRED, SORRY.                                              "};

char stemp[80];

/* Via stemp because neither end can be converted in place: callers pass string
 * literals, and screen RAM reads back as ROM when BASIC is banked in. */
static void to_stemp(const char* text, char length) {
    for (char i = 0; i < length; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') {
            c -= 0x60;
        } else if (c >= 'A' && c <= 'Z') {
            c -= 0x40;
        }
        stemp[i] = c;
    }
}

void write_line_len(const char* text, char column, char length) {
    if (length > (char)sizeof(stemp)) {
        length = (char)sizeof(stemp);
    }
    to_stemp(text, length);
    write_line_raw(stemp, column, length);
}

/* Step to the next line, scrolling when the last one is used up. */
void next_line(void) {
    screen_line_address += SCREEN_ROW_BYTES;
    if ((screen_line_address - SCREEN_ADDRESS) >= (24 * SCREEN_ROW_BYTES)) {
        screen_line_address -= SCREEN_ROW_BYTES;
        lcopy(SCREEN_ADDRESS + SCREEN_ROW_BYTES, SCREEN_ADDRESS, 23 * SCREEN_ROW_BYTES);
        lcopy(COLOUR_RAM_ADDRESS + SCREEN_ROW_BYTES, COLOUR_RAM_ADDRESS, 23 * SCREEN_ROW_BYTES);
        lfill(SCREEN_ADDRESS + 23 * SCREEN_ROW_BYTES, ' ', SCREEN_ROW_BYTES);
        lfill(COLOUR_RAM_ADDRESS + 23 * SCREEN_ROW_BYTES, SchemeText, SCREEN_ROW_BYTES);
    }
}

void write_line_raw(char* text, char column, char length) {
    lcopy((long)&text[0], screen_line_address + column, length);
    next_line();
}

/* Text on the current line without advancing, so the caller can read input
 * starting after it. */
void write_prompt(const char* text, char length) {
    to_stemp(text, length);
    lcopy((long)stemp, screen_line_address, length);
}

void write_line(const char* text, char column) {
    char length = 0;
    while (text[length] && length < 78) {
        length++;
    }
    write_line_len(text, column, length);
}

/* Recolour one field of the line just written, for callers that colour a line
 * piecewise rather than in one shade.  `colour` is a whole colour-RAM byte, so
 * an attribute such as AttribReverse can be OR'd into it. */
void recolour_last_line_segment(unsigned char column, unsigned char width, unsigned char colour) {
    /* Group the narrow terms: COLOUR_RAM_ADDRESS - SCREEN_ADDRESS folds at
     * compile time, leaving one 32-bit add rather than three. */
    long colour_address =
        COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + (screen_line_address - SCREEN_ROW_BYTES + column);
    lfill(colour_address, colour, width);
}

void recolour_last_line(char colour) {
    recolour_last_line_segment(0, SCREEN_ROW_BYTES, colour);
}

void display_footer(unsigned char index) {
    to_stemp(footer_messages[index], 80);
    lcopy((long)stemp, FOOTER_ADDRESS, 80);
    set_screen_attributes(FOOTER_ADDRESS, 80, AttribReverse);
}

void setup_screen(void) {
    unsigned char cia2_port_a;

    m65_io_enable();

    /* $D054 must precede the hot registers: their recalculation derives the
     * row width ($D058-$D059) from $D054's CHR16 bit, 80 bytes per row for
     * 8-bit characters and 160 for 16-bit. */

    // Normal 8-bit text mode
    VICIV.ctrlc = (VICIV.ctrlc & 0xa8) | 0x00;

    VICIV.ctrlb = 0xe0; // 80-column, fast CPU, extended attributes
    // 80 columns requires $D016 = $C9 to be properly positioned: bit 0 is the
    // H640 X-scroll correction.  Not $C8 -- that is the value the exit path
    // restores for 40 columns.
    VICIV.ctrl2 = 0xC9;
    VICIV.addr =
        (((CHARSET_ADDRESS - 0x8000U) >> 11) << 1) + (((SCREEN_ADDRESS - 0x8000U) >> 10) << 4);
    cia2_port_a = CIA2.pra;
    cia2_port_a &= 0xfc;
    cia2_port_a |= 0x01; // VIC RAM bank to $8000-$BFFF
    CIA2.pra = cia2_port_a;

    // Screen colours
    VICIV.bordercol = SchemeBorderDark;
    VICIV.screencol = SchemeBackground;

    // Clear screen RAM
    lfill(SCREEN_ADDRESS, 0x20, SCREEN_BYTES);

    clear_colour_ram();

    // Copy ASCII charset into place
    lcopy((int)&charset[0], CHARSET_ADDRESS, 0x800);

    // Set screen line address and write point
    screen_line_address = SCREEN_ADDRESS;
    screen_column = 0;

    display_footer(FooterCopyright);
}

void fatal_error(const unsigned char* filename, uint16_t line_number) {
    unsigned char i;
    display_footer(FooterFatal);
    for (i = 0; filename[i]; i++) {
        POKE(FOOTER_ADDRESS + 44 + i, filename[i]);
    }
    POKE(FOOTER_ADDRESS + 44 + i, ':');
    i++;
    screen_decimal(FOOTER_ADDRESS + 44 + i, line_number);
    lfill(COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + FOOTER_ADDRESS, SchemeError | AttribReverse, 80);
    for (;;) {
    }
}

void set_screen_attributes(long screen_address, unsigned char count, unsigned char attr) {
    // This involves setting colour RAM values, so we need to either LPOKE, or
    // map the 2KB colour RAM in at $D800 and work with it there.
    // XXX - For now we are LPOKING
    long addr = COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + screen_address;
    for (unsigned char i = 0; i < count; i++) {
        lpoke(addr, lpeek(addr) | attr);
        addr++;
    }
}

/* Read-modify-write the colour-RAM attribute byte of column `col` on the line
 * being edited: keep the `keep` bits of the current value and OR in `set`. */
static void set_attr(unsigned char column, unsigned char keep_mask, unsigned char set_mask) {
    long colour_address = COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + (column + screen_line_address);
    lpoke(colour_address, (lpeek(colour_address) & keep_mask) | set_mask);
}

char read_line(char* buffer, unsigned char max_length, unsigned char column) {
    char length = 0;
    /* The trailing key-clear reads this, and a max_length of 0 never enters the
     * loop that assigns it. */
    char c = 0;
    char reverse = 0x90;

    // Read input using hardware keyboard scanner

    // Flush keyboard input queue before reading input
    while (ASCIIKEY) {
        ASCIIKEY = 0;
    }

    while (length < max_length) {
        c = ASCIIKEY;

        // Show cursor
        set_attr(column + length, 0xf, reverse);

        if ((MODKEY & (MODKEY_MEGA | MODKEY_RSHIFT | MODKEY_LSHIFT)) >=
            (MODKEY_MEGA | MODKEY_LSHIFT)) {
            // C= + shift, so toggle case

            // Toggle upper/lower case font
            VICIV.addr = VICIV.addr ^ 0x02;

            while ((MODKEY & (MODKEY_MEGA | MODKEY_RSHIFT | MODKEY_LSHIFT)) >=
                (MODKEY_MEGA | MODKEY_LSHIFT)) {
            }
        }

        if (c) {

            if (c == KEY_F1) {
                /* Next colour scheme.  Nothing is redrawn and the line being
                 * typed is untouched: colour RAM holds palette entry numbers,
                 * so rewriting the entries repaints what is already there. */
                apply_scheme((uint8_t)(current_scheme + 1));
            } else if (c == 0x0e) {
                // Toggle upper/lower case font
                VICIV.addr = VICIV.addr ^ 0x02;
            } else if (c == 0x14) {
                // DELETE
                if (length) {
                    // Remove blink attribute from this char
                    set_attr(column + length, 0xf, 0);

                    // Go back one and erase
                    length--;
                    lpoke(screen_line_address + column + length, ' ');

                    // Re-enable blink for cursor
                    set_attr(column + length, 0xff, reverse);
                    buffer[length] = 0;
                }
            } else if (c == 0x0d) {
                buffer[length] = 0;

                // Hide cursor
                set_attr(column + length, 0xf, 0);

                /* Acknowledge RETURN as the character branch below does: a
                 * caller that reads again promptly would otherwise see it still
                 * held and take it as a second, empty line. */
                while (ASCIIKEY) {
                    ASCIIKEY = 1;
                }
                return length;
            } else {

                // Remove blink attribute from this char
                set_attr(column + length, 0xf, 0);
                buffer[length++] = c;

                // Mask char so that it looks right using screen codes instead of ASCII codes
                if (c > 0x40) {
                    c &= 0x1f;
                }
                lpoke(screen_line_address + column + length - 1, c);
            }

            // Clear keys from hardware keyboard scanner
            // XXX we clear all keys here, and work around a bug that causes crazy
            // fast key repeating. This can be turned back into acknowledging the
            // single key again later
            while (ASCIIKEY) {
                ASCIIKEY = 1;
            }
        }
    }

    // Hide cursor
    set_attr(column + length, 0xf, 0);

    // clear char from queue
    while (c && (ASCIIKEY == c)) {
        ASCIIKEY = 1;
    }

    return length;
}
