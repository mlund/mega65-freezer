/* The number formatting, which writes into buffers the caller owns.
 *
 * format.c touches no hardware, so the host can run it.  The oracle for the
 * digits is snprintf -- a separate implementation of the same conversion, so it
 * can disagree.  What snprintf does *not* define is what happens when a value
 * is too wide for the field: keeping the low digits is this code's own rule, so
 * those cases are pinned as literals rather than derived. */

#include <cstdio>
#include <string>

#include "doctest.h"

extern "C" {
#include "format.h"
}

namespace {

/* format_hex emits screen codes, where A-F are 1-6 rather than ASCII.
 * Rendering them back makes a failure message readable. */
char from_screen_code(char c) {
    const unsigned char v = static_cast<unsigned char>(c);
    if (v >= 0x30 && v <= 0x39) {
        return static_cast<char>(v);
    }
    if (v >= 1 && v <= 6) {
        return static_cast<char>('A' + v - 1);
    }
    return '?';
}

std::string readable(const char* codes, size_t length) {
    std::string out;
    for (size_t i = 0; i < length; i++) {
        out += from_screen_code(codes[i]);
    }
    return out;
}

std::string hex_of(long value, int columns) {
    char out[64] = {};
    format_hex(out, value, static_cast<char>(columns));
    return readable(out, static_cast<size_t>(columns));
}

std::string appended_hex(long value, uint8_t columns) {
    char out[64] = {};
    char* end = append_hex(out, value, columns);
    return readable(out, static_cast<size_t>(end - out));
}

std::string decimal_of(uint16_t value) {
    char out[64] = {};
    format_decimal(out, value);
    return std::string(out, out + DECIMAL_COLUMNS);
}

std::string appended_dec(uint16_t value) {
    char out[64] = {};
    *append_dec(out, value) = '\0';
    return out;
}

std::string appended_dec2(uint8_t value) {
    char out[64] = {};
    *append_dec2(out, value) = '\0';
    return out;
}

/* snprintf's answer, then this code's own rule for a value too wide for the
 * field: keep the low digits.  Stated once, checked against literals below. */
std::string wanted_hex(long value, int columns) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, "%0*lX", columns, value);
    const std::string all = buffer;
    return all.size() > static_cast<size_t>(columns) ? all.substr(all.size() - columns) : all;
}

std::string wanted_dec(int value, const char* form) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, form, value);
    return buffer;
}

}  // namespace

TEST_SUITE("format") {

/* One nybble, the digit every hex conversion is built from. */
TEST_CASE("a nybble") {
    for (int v = 0; v < 16; v++) {
        CAPTURE(v);
        char buffer[2];
        snprintf(buffer, sizeof buffer, "%X", v);
        CHECK(from_screen_code(static_cast<char>(nybl_to_screen(static_cast<uint8_t>(v))))
              == buffer[0]);
    }
}

/* Too wide for the field keeps the low digits, and that is this code's rule
 * rather than snprintf's -- so it is written out here. */
TEST_CASE("a value wider than its field keeps the low digits") {
    CHECK(hex_of(0x7FFFFFF, 1) == "F");
    CHECK(hex_of(0x12345, 2) == "45");
    CHECK(hex_of(0xFFFFFF, 4) == "FFFF");
    CHECK(hex_of(65535, 2) == "FF");
    CHECK(appended_hex(0x1234, 2) == "34");
}

TEST_CASE("hex at every width the tools use") {
    const long values[] = {0, 1, 9, 10, 15, 16, 255, 256, 4095, 4096, 65535,
                           0x12345, 0xFFFFFF, 0x7FFFFFF};
    for (long v : values) {
        for (int columns : {1, 2, 4, 7, 8}) {
            CAPTURE(v);
            CAPTURE(columns);
            CHECK(hex_of(v, columns) == wanted_hex(v, columns));
        }
    }
}

/* Five columns, left aligned with the padding after: the freezer writes these
 * straight after their labels, so a digit must land in the field's first
 * column, not its last. */
TEST_CASE("five-column decimal is left aligned") {
    CHECK(decimal_of(0) == "0    ");
    CHECK(decimal_of(1) == "1    ");
    CHECK(decimal_of(65535) == "65535");
    for (int v : {0, 1, 9, 10, 99, 100, 999, 1000, 9999, 10000, 65535}) {
        CAPTURE(v);
        CHECK(decimal_of(static_cast<uint16_t>(v)) == wanted_dec(v, "%-5d"));
    }
}

/* The appending forms, which compose MEGAINFO's lines. */
TEST_CASE("append_dec writes no padding") {
    for (int v : {0, 1, 9, 10, 99, 100, 1000, 2024, 65535}) {
        CAPTURE(v);
        CHECK(appended_dec(static_cast<uint16_t>(v)) == wanted_dec(v, "%d"));
    }
}

TEST_CASE("append_dec2 always writes two digits") {
    for (int v = 0; v < 100; v++) {
        CAPTURE(v);
        CHECK(appended_dec2(static_cast<uint8_t>(v)) == wanted_dec(v, "%02d"));
    }
}

TEST_CASE("append_hex") {
    for (long v : {0L, 0x5AL, 0xFFL, 0x1234L}) {
        for (uint8_t columns : {2, 4}) {
            CAPTURE(v);
            CAPTURE(columns);
            CHECK(appended_hex(v, columns) == wanted_hex(v, columns));
        }
    }
}

}  // TEST_SUITE
