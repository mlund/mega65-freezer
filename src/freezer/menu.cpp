/* The freeze menu's fixed text, converted to screen codes at compile time.
 *
 * This is the only C++ in the project.  It is here for the SDK's charset.h,
 * whose constexpr constructors carry the Unicode-to-screen-code tables, so the
 * conversion costs no code and a character the charset cannot represent is a
 * compile error rather than a wrong glyph.
 */

#include "colours.h"
#include "screen.h"
#include "version.h"

namespace {

/* The horizontal rules are not here: forty copies of one glyph is a fill, not a
 * copy, and storing four rows of them as fragments would cost 176 bytes against
 * a stream whose whole text is under 500.  They are painted with lfill_skip. */
constexpr auto FIXED = menu::stream(
    /* Dimmed: they name the program rather than saying anything about the
     * machine you froze. */
    menu::centered(0, SchemeTextDim, "MEGA65 FREEZE MENU " BUILD_TAG),
    menu::centered(1, SchemeTextDim, "(C) MUSEUM OF ELECTRONIC GAMES & ART"),

    /* The labels are dimmed and the values beside them are not: what is worth
     * reading here is the frozen machine's state, not the words naming it.  The
     * commands below stay bright, being the one thing on the screen to act on.
     *
     * The machine's settings.  Each value is written by its own Update path. */
    menu::fragment(1, 5, SchemeTextDim, "(C)PU MODE:"),
    menu::fragment(21, 5, SchemeTextDim, "(J)OY SWAP:"),
    menu::fragment(1, 6, SchemeTextDim, "CPU (F)REQ:"),
    menu::fragment(16, 6, SchemeTextDim, "MHZ"),
    menu::fragment(21, 6, SchemeTextDim, "CAR(T) ENABLE:"),
    menu::fragment(1, 7, SchemeTextDim, "C(R)T EMU:"),
    menu::fragment(21, 7, SchemeTextDim, "(V)IDEO:"),

    // The tools.
    menu::fragment(1, 9, SchemeText, "M - MONITOR"),
    menu::fragment(21, 9, SchemeText, "L - LOAD ROM/CHAR"),
    menu::fragment(1, 10, SchemeText, "A - AUDIO & VOLUME"),
    menu::fragment(21, 10, SchemeText, "F1 - THEME"),
    menu::fragment(21, 11, SchemeText, "D - FILEHOST"),
    menu::fragment(1, 11, SchemeText, "S - SPRITE EDITOR"),

    // The frozen program, to the right of the thumbnail.
    menu::fragment(21, 15, SchemeTextDim, "ROM:"),
    menu::fragment(21, 16, SchemeTextDim, "TASK ID:"),
    menu::fragment(21, 19, SchemeTextDim, "(0) INTERNAL DRIVE:"),
    menu::fragment(25, 20, SchemeTextDim, "(8) UNIT #"),
    menu::fragment(21, 22, SchemeTextDim, "(1) EXTERNAL 1565:"),
    menu::fragment(25, 23, SchemeTextDim, "(9) UNIT #"));

/* The disk chooser's standing text.  Separate streams because they appear at
 * different moments: the instructions whenever the list is redrawn, the notice
 * only while a directory is being read.
 *
 * Exactly the eighty cells of rows 23 and 24.  The runtime version carried
 * seventeen more characters than it ever drew. */
constexpr auto CHOOSER_HELP = menu::stream(menu::fragment(0,
    23,
    /* Cast because C++ deprecates a bitwise or between two enum types, where the
     * C the rest of the project is written in does not. */
    uint8_t(SchemeSelected) | uint8_t(AttribReverse),
    "  SELECT DISK IMAGE, THEN PRESS RETURN  "
    "  OR PRESS RUN/STOP TO LEAVE UNCHANGED  "));

constexpr auto CHOOSER_SCANNING =
    menu::stream(menu::fragment(9, 12, SchemeText, "SCANNING DIRECTORY ..."));

static_assert(CHOOSER_HELP.data[0] == 80, "the help fills both rows");

/* The title carries BUILD_TAG and so changes width between releases: check that
 * it still lands centred on row 0, rather than a column that would have to be
 * edited with every tag. Equal margins means length + 2 * column fills the row,
 * one short of it when an odd length cannot be split evenly. */
constexpr uint8_t TITLE_LENGTH = FIXED.data[0];
constexpr uint8_t TITLE_COLUMN = FIXED.data[1] / SCREEN_CELL_BYTES;
static_assert(FIXED.data[2] == 0, "title on row 0");
static_assert(TITLE_LENGTH + 2 * TITLE_COLUMN >= SCREEN_COLS - 1 &&
        TITLE_LENGTH + 2 * TITLE_COLUMN <= SCREEN_COLS,
    "title centred");

/* The bytes the tables are supposed to produce, checked where a mistake would
 * be a wrong glyph rather than a build failure. */
static_assert(FIXED.data[4] == 0x0D, "M");
static_assert(FIXED.data[5] == 0x05, "E");
static_assert(FIXED.data[9] == 0x35, "5, a digit, unchanged");
static_assert(FIXED.data[sizeof FIXED.data - 1] == 0, "stream terminator");

} // namespace

extern "C" const uint8_t* menu_fixed_stream(void) {
    return FIXED.data;
}

extern "C" const uint8_t* chooser_help_stream(void) {
    return CHOOSER_HELP.data;
}

extern "C" const uint8_t* chooser_scanning_stream(void) {
    return CHOOSER_SCANNING.data;
}
