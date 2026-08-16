/* FILEHOST's static text, converted to screen codes at compile time.  See
 * src/freezer/menu.cpp for the pattern this follows.
 *
 * Only the banner, the column headings and the key list are fixed; everything
 * between them is a record read off the card. */

#include "browser.h"
#include "colours.h"
#include "screen.h"

namespace {

constexpr auto FIXED = menu::stream(menu::fragment(0, 0, SchemeTextDim, "MEGA65 FILEHOST"),

    menu::fragment(COLUMN_TITLE, HEADING_Y, SchemeTextDim, "TITLE"),
    menu::fragment(COLUMN_AUTHOR, HEADING_Y, SchemeTextDim, "AUTHOR"),
    menu::fragment(COLUMN_CATEGORY, HEADING_Y, SchemeTextDim, "CATEGORY"),
    menu::fragment(COLUMN_YEAR, HEADING_Y, SchemeTextDim, "YEAR"),
    menu::fragment(COLUMN_KIND, HEADING_Y, SchemeTextDim, "TYPE"),

    menu::fragment(0,
        FOOTER_Y,
        SchemeText,
        "RETURN-ATTACH  /-FIND  S-SORT  F-FETCH  T-SERVER  RUN/STOP-BACK"));

static_assert(FIXED.data[0] == 15, "the banner is the first fragment");
static_assert(FIXED.data[sizeof FIXED.data - 1] == 0, "stream terminator");

} // namespace

extern "C" const uint8_t* menu_fixed_stream(void) {
    return FIXED.data;
}
