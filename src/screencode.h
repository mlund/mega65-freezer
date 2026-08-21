#pragma once

#include <stdint.h>

/* Printable ASCII onto the C64 ROM charset the tools draw with.
 *
 * Text that came off the card is ASCII -- FAT filenames, and the FileHost
 * catalogue, which is deliberately not PETSCII so that the machine is not the
 * only thing that can render it.  draw_text() and petscii_to_screen() both read
 * their input as PETSCII, where a lowercase letter is a different code
 * entirely, so ASCII text drawn through either comes out as debris.
 *
 * The screen is uppercase-only, so both cases of a letter give the same glyph.
 * Anything the charset cannot show becomes a question mark rather than whatever
 * the byte happens to index -- including the backslash and caret, whose codes
 * here are a pound sign and an up arrow.  The underscore is the exception: it
 * is drawn from elsewhere in the charset, where a glyph for it does exist.
 *
 * No hardware, so src/screencode.c compiles for the host and is tested there. */
uint8_t ascii_to_screen(uint8_t c);
