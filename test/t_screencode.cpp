/* ASCII onto the C64 ROM charset.
 *
 * The oracle is the charset itself: screen code n is the glyph n cells into
 * the ROM, and that order -- @, the letters, then the punctuation at $1B-$1F,
 * then the ASCII-shaped run from space -- is what the checks below state.
 * Every byte in the catalogue's range is covered, because the failure this
 * guards against is one character in a title rendering as debris. */

#include "doctest.h"

extern "C" {
#include "screencode.h"
}

namespace {

constexpr uint8_t QUESTION_MARK = 0x3F;

}  // namespace

TEST_SUITE("screencode") {

/* Both cases give the same glyph: the screen is uppercase-only, and the
 * catalogue is full of mixed-case titles. */
TEST_CASE("letters fold to the uppercase glyphs at 1 to 26") {
    for (uint8_t i = 0; i < 26; i++) {
        CAPTURE(i);
        CHECK(ascii_to_screen(static_cast<uint8_t>('A' + i)) == i + 1);
        CHECK(ascii_to_screen(static_cast<uint8_t>('a' + i)) == i + 1);
    }
}

TEST_CASE("digits and punctuation keep their ASCII value") {
    for (uint8_t c = 0x20; c < 0x40; c++) {
        CAPTURE(c);
        CHECK(ascii_to_screen(c) == c);
    }
}

TEST_CASE("the glyphs around the letters") {
    CHECK(ascii_to_screen('@') == 0x00);
    CHECK(ascii_to_screen('[') == 0x1B);
    CHECK(ascii_to_screen(']') == 0x1D);
}

/* $1C, $1E and $1F are a pound sign, an up arrow and a left arrow -- not the
 * ASCII characters whose codes would land there.  The charset has no glyph for
 * these three, so they take the fallback: an underscore rendering as an arrow
 * would be exactly the wrong-glyph-for-the-byte this converter exists to
 * prevent. */
TEST_CASE("the three the charset cannot show are not faked") {
    CHECK(ascii_to_screen('\\') == QUESTION_MARK);
    CHECK(ascii_to_screen('^') == QUESTION_MARK);
    CHECK(ascii_to_screen('_') == QUESTION_MARK);
}

/* The catalogue promises 0x20-0x7E, so anything outside it is a file the
 * renderer got wrong -- shown as a question mark rather than as whatever glyph
 * the byte happens to select. */
TEST_CASE("anything outside printable ASCII becomes a question mark") {
    CHECK(ascii_to_screen(0x00) == QUESTION_MARK);
    CHECK(ascii_to_screen(0x1F) == QUESTION_MARK);
    CHECK(ascii_to_screen(0x7F) == QUESTION_MARK);
    CHECK(ascii_to_screen(0x80) == QUESTION_MARK);
    CHECK(ascii_to_screen(0xFF) == QUESTION_MARK);
}

/* The rest of printable ASCII: past `z` the charset has nothing, and the
 * backtick before `a` is not the `@` its code neighbours suggest. */
TEST_CASE("the printable bytes with no glyph") {
    for (uint8_t c : {'`', '{', '|', '}', '~'}) {
        CAPTURE(c);
        CHECK(ascii_to_screen(c) == QUESTION_MARK);
    }
}

}  // TEST_SUITE
