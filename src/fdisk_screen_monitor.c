#include "fdisk_screen_monitor.h"

#include "ascii.h"
#include "fdisk_memory.h"
#include "fdisk_screen.h"

extern unsigned char* charset;

char* footer_messages[FOOTER_MAX + 1] = {
    "MEGA65 FREEZE MONITOR V00.01 :     X - RETURN TO FREEZE MENU, M - DISPLAY MEMORY",
    "MEGA65 SPRITE EDITOR V00.01 :        1 - CLEAR, 2- DRAW ETC, H - HELP, F3 - EXIT",
    "                                                                                ",
    "A FATAL ERROR HAS OCCURRED, SORRY.                                              "};

char stemp[80];

/* Converts into stemp rather than in place: callers pass string literals, and
 * modifying those is undefined behaviour. */
void write_line_len(const char* s, char col, char length) {
    char i;
    if (length > (char)sizeof(stemp))
        length = (char)sizeof(stemp);
    for (i = 0; i < length; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z')
            c -= 0x60;
        else if (c >= 'A' && c <= 'Z')
            c -= 0x40;
        stemp[i] = c;
    }
    write_line_raw(stemp, col, length);
}

void write_line_raw(char* s, char col, char length) {
    lcopy((long)&s[0], screen_line_address + col, length);
    screen_line_address += 80;
    if ((screen_line_address - SCREEN_ADDRESS) >= (24 * 80)) {
        screen_line_address -= 80;
        lcopy(SCREEN_ADDRESS + 80, SCREEN_ADDRESS, 23 * 80);
        lcopy(COLOUR_RAM_ADDRESS + 80, COLOUR_RAM_ADDRESS, 23 * 80);
        lfill(SCREEN_ADDRESS + 23 * 80, ' ', 80);
        lfill(COLOUR_RAM_ADDRESS + 23 * 80, 1, 80);
    }
}

void write_line(const char* s, char col) {
    char len = 0;
    while (s[len] && len < 78)
        len++;
    write_line_len(s, col, len);
}

void recolour_last_line(char colour) {
    long colour_address = COLOUR_RAM_ADDRESS + (screen_line_address - SCREEN_ADDRESS) - 80;
    lfill(colour_address, colour, 80);
    return;
}

long addr;
void display_footer(unsigned char index) {
    char i;
    /* Convert into a buffer, then copy once.  Converting in place at
     * FOOTER_ADDRESS meant reading $BF80 back, and that read returns BASIC ROM
     * rather than screen RAM whenever BASIC is banked in, so nothing matched
     * 'A'-'Z' and the line stayed in ASCII. */
    for (i = 0; i < 80; i++) {
        char c = footer_messages[index][i];
        if (c >= 'a' && c <= 'z')
            c -= 0x60;
        else if (c >= 'A' && c <= 'Z')
            c -= 0x40;
        stemp[i] = c;
    }
    lcopy((long)stemp, FOOTER_ADDRESS, 80);
    set_screen_attributes(FOOTER_ADDRESS, 80, ATTRIB_REVERSE);
}

void setup_screen(void) {
    unsigned char v;

    m65_io_enable();

    /* Hot registers first, then the VIC-IV ones with propagation off: any hot
     * write after this point would recalculate them away. */
    POKE(0xD031U, 0xe0); // 80-column, fast CPU, extended attributes
    POKE(0xD016U, 0xC8); // 80 columns needs this for correct positioning
    POKE(0xD018U,
        (((CHARSET_ADDRESS - 0x8000U) >> 11) << 1) + (((SCREEN_ADDRESS - 0x8000U) >> 10) << 4));
    v = PEEK(0xDD00U);
    v &= 0xfc;
    v |= 0x01; // VIC RAM bank to $8000-$BFFF
    POKE(0xDD00U, v);

    VIC4_LOCK_ACQUIRE();

    // Normal 8-bit text mode
    POKE(0xD054U, (PEEK(0xD054) & 0xa8) | 0x00);

    // Screen colours
    POKE(0xD020U, 0);
    POKE(0xD021U, 6);

    // Clear screen RAM
    lfill(SCREEN_ADDRESS, 0x20, 2000);

    // Clear colour RAM: white text
    lfill(0x1f800, 0x01, 2000);

    // Copy ASCII charset into place
    lcopy((int)&charset[0], CHARSET_ADDRESS, 0x800);

    // Set screen line address and write point
    screen_line_address = SCREEN_ADDRESS;
    screen_column = 0;

    display_footer(FOOTER_COPYRIGHT);
}

void screen_colour_line(unsigned char line, unsigned char colour) {
    // Set colour RAM for this screen line to this colour
    // (use bit-shifting as fast alternative to multiply)
    lfill(0x1f800 + (line << 6) + (line << 4), colour, 80);
}

static unsigned char i;

void fatal_error(unsigned char* filename, unsigned int line_number) {
    display_footer(FOOTER_FATAL);
    for (i = 0; filename[i]; i++)
        POKE(FOOTER_ADDRESS + 44 + i, filename[i]);
    POKE(FOOTER_ADDRESS + 44 + i, ':');
    i++;
    screen_decimal(FOOTER_ADDRESS + 44 + i, line_number);
    lfill(COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + FOOTER_ADDRESS, 2 | ATTRIB_REVERSE, 80);
    for (;;)
        continue;
}

void set_screen_attributes(long p, unsigned char count, unsigned char attr) {
    // This involves setting colour RAM values, so we need to either LPOKE, or
    // map the 2KB colour RAM in at $D800 and work with it there.
    // XXX - For now we are LPOKING
    long addr = COLOUR_RAM_ADDRESS - SCREEN_ADDRESS + p;
    for (i = 0; i < count; i++) {
        lpoke(addr, lpeek(addr) | attr);
        addr++;
    }
}

char read_line(char* buffer, unsigned char maxlen) {
    char len = 0;
    char c;
    char reverse = 0x90;

    // Read input using hardware keyboard scanner

    // Flush keyboard input queue before reading input
    while (PEEK(0xD610U))
        POKE(0xD610U, 0);

    while (len < maxlen) {
        c = PEEK(0xD610U); // read char

#if 0
    reverse ^=0x20;
#endif

        // Show cursor
        lpoke(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS,
            reverse |
                (lpeek(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS) & 0xf));

        if ((PEEK(0xD611U) & 0x0b) >= 0x09) {
            // C= + shift, so toggle case

            // Toggle upper/lower case font
            POKE(0xD018U, PEEK(0xD018U) ^ 0x02);

            while ((PEEK(0xD611U) & 0x0b) >= 0x09)
                continue;
        }

        if (c) {

            if (c == 0x0e) {
                // Toggle upper/lower case font
                POKE(0xD018U, PEEK(0xD018U) ^ 0x02);
            } else if (c == 0x14) {
                // DELETE
                if (len) {
                    // Remove blink attribute from this char
                    lpoke(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS,
                        lpeek(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS) &
                            0xf);

                    // Go back one and erase
                    len--;
                    lpoke(screen_line_address + len, ' ');

                    // Re-enable blink for cursor
                    lpoke(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS,
                        lpeek(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS) |
                            reverse);
                    buffer[len] = 0;
                }
            } else if (c == 0x0d) {
                buffer[len] = 0;

                // Hide cursor
                lpoke(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS,
                    0x00 |
                        (lpeek(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS) &
                            0xf));

                return len;
            } else {

                // Remove blink attribute from this char
                lpoke(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS,
                    lpeek(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS) & 0xf);
                buffer[len++] = c;

                // Mask char so that it looks right using screen codes instead of ASCII codes
                if (c > 0x40)
                    c &= 0x1f;
                lpoke(screen_line_address + len - 1, c);
            }

            // Clear keys from hardware keyboard scanner
            // XXX we clear all keys here, and work around a bug that causes crazy
            // fast key repeating. This can be turned back into acknowledging the
            // single key again later
            while (PEEK(0xD610U)) {
                POKE(0xD610U, 1);
            }
        }
    }

    // Hide cursor
    lpoke(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS,
        0x00 | (lpeek(len + screen_line_address + COLOUR_RAM_ADDRESS - SCREEN_ADDRESS) & 0xf));

    // clear char from queue
    while (c && (PEEK(0xD610U) == c))
        POKE(0xD610U, 1);

    return len;
}
