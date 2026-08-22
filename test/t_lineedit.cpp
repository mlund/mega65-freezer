/* SPRITED's line editor, typed at directly.
 *
 * The editor asks for a filename and for a yes/no answer.  Driving that through
 * the emulator means a key at a time and a screen read to see the result, so
 * the cases that actually go wrong -- backspacing an empty line, the last cell,
 * a control code -- are the ones least likely to get typed.  The logic is pure,
 * so it runs here instead.
 *
 * Expected values are written out rather than computed: an oracle that built
 * them the way the editor does could not disagree with it. */

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

extern "C" {
#include "lineedit.h"
}

namespace {

constexpr uint8_t RETURN = 0x0D;
constexpr uint8_t DELETE = 0x14;

struct Line {
    uint8_t length;
    bool done;
    std::string text;   /* up to the terminator */
    bool terminated;
};

/* Type `keys` into a fresh buffer of `capacity`, stopping where the editor
 * says the line is finished.  The buffer is poisoned first, so a missing
 * terminator shows as a failure rather than as whatever was already zero. */
Line typed(uint8_t capacity, const std::vector<uint8_t>& keys) {
    char buffer[256];
    memset(buffer, '\xAA', sizeof buffer);
    buffer[0] = '\0';

    uint8_t length = 0;
    bool done = false;
    for (uint8_t key : keys) {
        if (done) {
            break;
        }
        done = line_edit(buffer, capacity, &length, key);
    }
    return {length, done, std::string(buffer, buffer + length),
            buffer[length] == '\0'};
}

std::vector<uint8_t> chars(const std::string& text) {
    return {text.begin(), text.end()};
}

}  // namespace

TEST_SUITE("lineedit") {

TEST_CASE("an ordinary line") {
    SUBCASE("RETURN ends it") {
        const Line line = typed(16, chars("HELLO\r"));
        CHECK(line.length == 5);
        CHECK(line.done);
        CHECK(line.text == "HELLO");
        CHECK(line.terminated);
    }

    /* Without RETURN the line is unfinished and still terminated: a redraw
     * happens between every key, and it reads the buffer as a string. */
    SUBCASE("without RETURN it is unfinished but still terminated") {
        const Line line = typed(16, chars("HELLO"));
        CHECK(line.length == 5);
        CHECK_FALSE(line.done);
        CHECK(line.text == "HELLO");
        CHECK(line.terminated);
    }

    /* RETURN wins wherever it appears, and stops the line there. */
    SUBCASE("RETURN first leaves the line empty") {
        const Line line = typed(16, {RETURN, 'A'});
        CHECK(line.length == 0);
        CHECK(line.done);
        CHECK(line.text.empty());
        CHECK(line.terminated);
    }
}

TEST_CASE("backspace") {
    SUBCASE("removes one and re-terminates") {
        const Line line = typed(16, {'H', 'E', 'L', 'L', 'O', DELETE});
        CHECK(line.length == 4);
        CHECK(line.text == "HELL");
        CHECK(line.terminated);
    }

    /* Backspacing an empty line does nothing rather than running off the
     * front, which is the bug this shape of code usually has. */
    SUBCASE("an empty line stays empty") {
        const Line line = typed(16, {DELETE, DELETE, DELETE});
        CHECK(line.length == 0);
        CHECK(line.terminated);
    }

    SUBCASE("past empty stays empty") {
        const Line line = typed(16, {'A', DELETE, DELETE});
        CHECK(line.length == 0);
        CHECK(line.terminated);
    }
}

/* Capacity includes the terminator, so capacity 4 holds three characters. */
TEST_CASE("capacity") {
    SUBCASE("three characters fit in four") {
        const Line line = typed(4, chars("ABC"));
        CHECK(line.length == 3);
        CHECK(line.text == "ABC");
        CHECK(line.terminated);
    }

    SUBCASE("the fourth is dropped rather than overrunning") {
        const Line line = typed(4, chars("ABCD"));
        CHECK(line.length == 3);
        CHECK(line.text == "ABC");
        CHECK(line.terminated);
    }

    /* Having been refused, backspace still works: the length was not advanced
     * past the end. */
    SUBCASE("backspace after full still removes one") {
        const Line line = typed(4, {'A', 'B', 'C', 'D', DELETE});
        CHECK(line.length == 2);
        CHECK(line.text == "AB");
        CHECK(line.terminated);
    }

    SUBCASE("a capacity of one is all terminator") {
        const Line line = typed(1, {'A'});
        CHECK(line.length == 0);
        CHECK(line.terminated);
    }
}

TEST_CASE("what a key may be") {
    /* conio's cinput() with CINPUT_ACCEPT_ALL takes control codes, which would
     * put one in a filename. */
    SUBCASE("control codes are refused") {
        const Line line = typed(16, {0x00, 0x01, 0x1F, 0x7F, 0x93});
        CHECK(line.length == 0);
        CHECK(line.terminated);
    }

    SUBCASE("space and the printable extremes are accepted") {
        const Line line = typed(16, {0x20, 0x7E});
        CHECK(line.length == 2);
        CHECK(line.text == " ~");
        CHECK(line.terminated);
    }
}

}  // TEST_SUITE
