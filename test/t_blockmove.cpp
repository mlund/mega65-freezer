/* The block operations, driven against a flat array.
 *
 * blockmove.c takes its two memory accesses from the caller, so on the host
 * they become an array and the logic runs without a MEGA65.  Accesses outside
 * it fail, which is how the unmapped paths are reached.
 *
 * The oracle is std::memmove: a separate implementation of the same job, so it
 * can disagree with block_move().  The overlap cases are the point -- a forward
 * copy into a destination inside the source range corrupts only the tail, which
 * reads as working on hardware. */

#include <cstring>
#include <vector>

#include "doctest.h"

extern "C" {
#include "blockmove.h"
}

namespace {

/* Larger than one chunk, so multi-chunk paths are reachable, and small enough
 * that running off the end is easy to arrange. */
constexpr uint32_t MEMORY_SIZE = 2048;
unsigned char memory[MEMORY_SIZE];

/* Not i & 0xff: a byte that repeats every 256 would hide an off-by-one that
 * lands a chunk boundary away. */
void fill_pattern(unsigned char* into) {
    for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
        into[i] = static_cast<unsigned char>((i * 7u + 3u) ^ (i >> 5));
    }
}

std::vector<unsigned char> patterned() {
    std::vector<unsigned char> out(MEMORY_SIZE);
    fill_pattern(out.data());
    return out;
}

}  // namespace

extern "C" bool disasm_read_byte(uint32_t address, uint8_t* value) {
    if (address >= MEMORY_SIZE) {
        return false;
    }
    *value = memory[address];
    return true;
}

extern "C" bool block_write_byte(uint32_t address, unsigned char value) {
    if (address >= MEMORY_SIZE) {
        return false;
    }
    memory[address] = value;
    return true;
}

TEST_SUITE("blockmove") {

TEST_CASE("a move agrees with memmove") {
    fill_pattern(memory);

    uint32_t from = 0, to = 0, length = 0;

    /* Disjoint, within one chunk and across several. */
    SUBCASE("disjoint, inside a chunk")      { from = 0;   to = 512;  length = 16; }
    SUBCASE("disjoint, across chunks")       { from = 0;   to = 512;  length = 300; }
    SUBCASE("disjoint, long")                { from = 100; to = 900;  length = 600; }
    /* Overlapping upward: the case a forward copy corrupts. */
    SUBCASE("overlapping up, short")         { from = 100; to = 110;  length = 64; }
    SUBCASE("overlapping up, across chunks") { from = 100; to = 110;  length = 300; }
    SUBCASE("overlapping up, wide")          { from = 0;   to = 255;  length = 512; }
    /* Overlapping downward. */
    SUBCASE("overlapping down, short")       { from = 110; to = 100;  length = 64; }
    SUBCASE("overlapping down, long")        { from = 400; to = 100;  length = 600; }
    /* Exactly a chunk, and one either side of it. */
    SUBCASE("one below a chunk")             { from = 0;   to = 1000; length = BLOCK_MAX - 1; }
    SUBCASE("exactly a chunk")               { from = 0;   to = 1000; length = BLOCK_MAX; }
    SUBCASE("one above a chunk")             { from = 0;   to = 1000; length = BLOCK_MAX + 1; }
    /* Degenerate. */
    SUBCASE("nothing to move")               { from = 0;   to = 1000; length = 0; }
    SUBCASE("one byte")                      { from = 0;   to = 1000; length = 1; }
    SUBCASE("onto itself")                   { from = 500; to = 500;  length = 100; }

    std::vector<unsigned char> want = patterned();
    memmove(want.data() + to, want.data() + from, length);

    REQUIRE(block_move(from, to, length));
    CHECK(memcmp(memory, want.data(), MEMORY_SIZE) == 0);
}

/* Off the end must report failure rather than wrap or walk past the array. */
TEST_CASE("an access off the end fails") {
    fill_pattern(memory);

    SUBCASE("source runs off the end") {
        CHECK_FALSE(block_move(MEMORY_SIZE - 8, 0, 32));
    }
    SUBCASE("destination runs off the end") {
        CHECK_FALSE(block_move(0, MEMORY_SIZE - 8, 32));
    }
    SUBCASE("source starts past the end") {
        CHECK_FALSE(block_move(MEMORY_SIZE + 16, 0, 4));
    }
}

TEST_CASE("block_read clamps and stops at the first failed byte") {
    fill_pattern(memory);
    const std::vector<unsigned char> want = patterned();

    SUBCASE("more than a chunk is capped at one chunk") {
        CHECK(block_read(0, 1000) == BLOCK_MAX);
        CHECK(memcmp(block_buffer, want.data(), BLOCK_MAX) == 0);
    }

    SUBCASE("straddling the end returns only the mapped part") {
        CHECK(block_read(MEMORY_SIZE - 10, 32) == 10);
        CHECK(memcmp(block_buffer, want.data() + MEMORY_SIZE - 10, 10) == 0);
    }
}

}  // TEST_SUITE
