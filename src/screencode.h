#pragma once

#include <stdint.h>

/* Printable ASCII onto the C64 ROM charset the tools draw with.
 *
 * Text off the card is ASCII -- FAT filenames and the FileHost catalogue --
 * while draw_text() and petscii_to_screen() read PETSCII, where a lowercase
 * letter is a different code entirely, so ASCII through either is debris.
 *
 * The screen is uppercase-only, so both cases give one glyph.  What the
 * charset cannot show becomes a question mark rather than whatever the byte
 * indexes, backslash and caret included; the underscore is the exception,
 * having a glyph elsewhere in the charset.
 *
 * No hardware, so screencode.c compiles for the host and is tested there. */
uint8_t ascii_to_screen(uint8_t c);
