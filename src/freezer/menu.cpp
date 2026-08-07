/* The freeze menu's fixed text, converted to screen codes at compile time.
 *
 * This is the only C++ in the project.  It is here for the SDK's charset.h,
 * whose constexpr constructors carry the Unicode-to-screen-code tables, so the
 * conversion costs no code and a character the charset cannot represent is a
 * compile error rather than a wrong glyph.
 */

#include "colours.h"
#include "screen.h"

namespace {

/* The horizontal rules are not here: forty copies of one glyph is a fill, not a
 * copy, and storing four rows of them as fragments would cost 176 bytes against
 * a stream whose whole text is under 500.  They are painted with lfill_skip. */
constexpr auto FIXED = menu::stream(
    /* Dimmed: they name the program rather than saying anything about the
     * machine you froze. */
    menu::fragment(6, 0, SchemeTextDim, "MEGA65 FREEZE MENU V0.4.1DEV"),
    menu::fragment(2, 1, SchemeTextDim, "(C) MUSEUM OF ELECTRONIC GAMES & ART"),

    // The machine's settings.  Each value is written by its own Update path.
    menu::fragment(1, 5, SchemeText, "(C)PU MODE:"),
    menu::fragment(21, 5, SchemeText, "(J)OY SWAP:"),
    menu::fragment(1, 6, SchemeText, "CPU (F)REQ:"),
    menu::fragment(16, 6, SchemeText, "MHZ"),
    menu::fragment(21, 6, SchemeText, "CAR(T) ENABLE:"),
    menu::fragment(1, 7, SchemeText, "C(R)T EMU:"),
    menu::fragment(21, 7, SchemeText, "(V)IDEO:"),

    // The tools.
    menu::fragment(1, 9, SchemeText, "M - MONITOR"),
    menu::fragment(21, 9, SchemeText, "L - LOAD ROM/CHAR"),
    menu::fragment(1, 10, SchemeText, "A - AUDIO & VOLUME"),
    menu::fragment(1, 11, SchemeText, "S - SPRITE EDITOR"),

    // The frozen program, to the right of the thumbnail.
    menu::fragment(21, 15, SchemeText, "ROM:"),
    menu::fragment(21, 16, SchemeText, "TASK ID:"),
    menu::fragment(21, 19, SchemeText, "(0) INTERNAL DRIVE:"),
    menu::fragment(25, 20, SchemeText, "(8) UNIT #"),
    menu::fragment(21, 22, SchemeText, "(1) EXTERNAL 1565:"),
    menu::fragment(25, 23, SchemeText, "(9) UNIT #"));

/* The bytes the tables are supposed to produce, checked where a mistake would
 * be a wrong glyph rather than a build failure. */
static_assert(FIXED.data[0] == 28, "title length");
/* Column 6 of row 0, as a cell offset: 0 * 80 + 6 * 2. */
static_assert(FIXED.data[1] == 12 && FIXED.data[2] == 0, "title position");
static_assert(FIXED.data[4] == 0x0D, "M");
static_assert(FIXED.data[5] == 0x05, "E");
static_assert(FIXED.data[9] == 0x35, "5, a digit, unchanged");
static_assert(FIXED.data[sizeof FIXED.data - 1] == 0, "stream terminator");

} // namespace

extern "C" const uint8_t* menu_fixed_stream(void) {
    return FIXED.data;
}
