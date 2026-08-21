/* Which ASCII codes the charset can show, and where in it each one sits. */

#include "screencode.h"

uint8_t ascii_to_screen(uint8_t c) {
    if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
    }
    /* The underscore does have a glyph, just not at its own code: $1F there is
     * a left arrow, while $64 in the graphics run is a rule along the bottom
     * row and nothing else -- an underscore in all but name.  Worth the one
     * case because upstream's authors and filenames are full of them, and
     * `jim_64` drawn as `JIM?64` reads as a broken catalogue rather than as a
     * charset that is missing a character. */
    if (c == '_') {
        return 0x64;
    }
    /* The charset runs @, A-Z, [ and then glyphs that are not the ASCII
     * characters at those codes: $1C is a pound sign and $1E an up arrow,
     * where ASCII has \ and ^.  Neither has a glyph anywhere, so they are
     * rejected with everything outside the printable range rather than drawn
     * as the symbol that happens to sit at the code. */
    if (c < ' ' || c > ']' || c == '\\') {
        return '?';
    }
    /* @ to ] is ASCII less $40; the run from space is ASCII itself. */
    return c >= '@' ? (uint8_t)(c - '@') : c;
}
