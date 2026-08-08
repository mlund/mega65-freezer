/* AUDIOMIX's static text, converted to screen codes at compile time.
 *
 * Unlike FREEZER's menu this screen is mostly a live hex matrix / dB meter,
 * so only the labels are fragments -- the coefficient matrix, the dB bars
 * and the row-3 readouts stay runtime-drawn.  See src/freezer/menu.cpp for
 * the pattern this follows.
 */

#include "colours.h"
#include "screen.h"

namespace {

/* Title and copyright: identical text in both views, so stored once. */
constexpr auto BANNER = menu::stream(menu::fragment(9, 0, SchemeTextDim, "MEGA65 AUDIO MIXER MENU"),
    menu::fragment(2, 1, SchemeTextDim, "(C) FLINDERS UNI, M.E.G.A. 2018-2024"));

/* The advanced (hex matrix) view.  Row 3 columns 0-7 are a live cursor/value
 * readout, drawn separately every redraw -- only its column-8+ header tail
 * is static.  Rows 5-20's labels stop at column 7; columns 8-39 are the
 * live matrix. */
constexpr auto ADVANCED_FIXED =
    menu::stream(menu::fragment(8, 3, SchemeText, "LFT RGT PH1 PH2 BTL BTR HDL HDR"),

        menu::fragment(3, 5, SchemeText, U"SIDL\U0001FB72"),
        menu::fragment(3, 6, SchemeText, U"SIDR\U0001FB72"),
        menu::fragment(1, 7, SchemeText, U"PHONE1\U0001FB72"),
        menu::fragment(1, 8, SchemeText, U"PHONE2\U0001FB72"),
        menu::fragment(0, 9, SchemeText, U"BTOOTHL\U0001FB72"),
        menu::fragment(0, 10, SchemeText, U"BTOOTHR\U0001FB72"),
        menu::fragment(0, 11, SchemeText, U"LINEINL\U0001FB72"),
        menu::fragment(0, 12, SchemeText, U"LINEINR\U0001FB72"),
        menu::fragment(2, 13, SchemeText, U"DIGIL\U0001FB72"),
        menu::fragment(2, 14, SchemeText, U"DIGIR\U0001FB72"),
        menu::fragment(2, 15, SchemeText, U"MIC0L\U0001FB72"),
        menu::fragment(2, 16, SchemeText, U"MIC0R\U0001FB72"),
        menu::fragment(2, 17, SchemeText, U"MIC1L\U0001FB72"),
        menu::fragment(2, 18, SchemeText, U"MIC1R\U0001FB72"),
        menu::fragment(1, 19, SchemeText, U"OPL FM\U0001FB72"),
        menu::fragment(1, 20, SchemeText, U"MASTER\U0001FB72"),

        menu::fragment(1, 22, SchemeText, "T - TEST SOUND, CURSOR KEYS - NAVIGATE"),
        menu::fragment(1, 23, SchemeText, "+/- ADJUST VALUE,    0/* - FAST ADJUST"),
        menu::fragment(1, 24, SchemeText, "F3 - SIMPLE MODE,  M - TOGGLE MIC MUTE"));

/* The simple (dB-bar) view.  Row 3 is blank in both views -- nothing draws
 * it.  Rows 6-11/15-20's labels stop at column 10; columns 11+ are the live
 * bar and its dB annotation.  Both channel headings end flush at column 28
 * (RIGHT is one letter longer than LEFT, so it starts one column earlier) --
 * kept as in the original rather than independently centred. */
constexpr auto SIMPLE_FIXED = menu::stream(menu::fragment(9, 4, SchemeText, "LEFT OUTPUT CHANNEL:"),
    menu::fragment(4, 6, SchemeText, U"MASTER\U0001FB72"),
    menu::fragment(1, 7, SchemeText, U"L SID 3+4\U0001FB72"),
    menu::fragment(1, 8, SchemeText, U"R SID 1+2\U0001FB72"),
    menu::fragment(1, 9, SchemeText, U"LEFT DIGI\U0001FB72"),
    menu::fragment(0, 10, SchemeText, U"RIGHT DIGI\U0001FB72"),
    menu::fragment(0, 11, SchemeText, U"SFX OPL FM\U0001FB72"),

    menu::fragment(8, 13, SchemeText, "RIGHT OUTPUT CHANNEL:"),
    menu::fragment(4, 15, SchemeText, U"MASTER\U0001FB72"),
    menu::fragment(1, 16, SchemeText, U"L SID 3+4\U0001FB72"),
    menu::fragment(1, 17, SchemeText, U"R SID 1+2\U0001FB72"),
    menu::fragment(1, 18, SchemeText, U"LEFT DIGI\U0001FB72"),
    menu::fragment(0, 19, SchemeText, U"RIGHT DIGI\U0001FB72"),
    menu::fragment(0, 20, SchemeText, U"SFX OPL FM\U0001FB72"),

    menu::fragment(1, 22, SchemeText, "T - TEST SOUND, CURSOR KEYS - NAVIGATE"),
    menu::fragment(1, 23, SchemeText, "+/- VOL, S - STEREO/MONO, W - SWAP L/R"),
    menu::fragment(1, 24, SchemeText, "F3 - EXIT, M - MUTE, A - ADVANCED MODE"));

static_assert(BANNER.data[0] == 23, "title length");
static_assert(BANNER.data[sizeof BANNER.data - 1] == 0, "stream terminator");
static_assert(ADVANCED_FIXED.data[sizeof ADVANCED_FIXED.data - 1] == 0, "stream terminator");
static_assert(SIMPLE_FIXED.data[sizeof SIMPLE_FIXED.data - 1] == 0, "stream terminator");

} // namespace

extern "C" const uint8_t* audiomix_banner_stream(void) {
    return BANNER.data;
}
extern "C" const uint8_t* audiomix_advanced_fixed_stream(void) {
    return ADVANCED_FIXED.data;
}
extern "C" const uint8_t* audiomix_simple_fixed_stream(void) {
    return SIMPLE_FIXED.data;
}
