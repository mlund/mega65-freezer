/* Which ASCII codes the charset can show, and where in it each one sits. */

#include "screencode.h"

uint8_t ascii_to_screen(uint8_t c) {
    if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
    }
    /* The charset runs @, A-Z, [ and then three glyphs that are not the ASCII
     * characters at those codes: $1C is a pound sign, $1E an up arrow and $1F a
     * left arrow, where ASCII has \, ^ and _.  Those three have no glyph at
     * all, so they are rejected with everything outside the printable range
     * rather than drawn as the symbol that happens to sit at the code. */
    if (c < ' ' || c > ']' || c == '\\') {
        return '?';
    }
    /* @ to ] is ASCII less $40; the run from space is ASCII itself. */
    return c >= '@' ? (uint8_t)(c - '@') : c;
}
