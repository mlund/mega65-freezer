/* Whether a program may be put into the frozen machine, and exactly where.
 *
 * All of it is arithmetic and rules, so none of it needs a machine: what a
 * load address means, which BASIC is running behind it, and which pointer has
 * to be told where the program ends.  The writing itself is hardware and is
 * checked by fetching a small program on the machine and resuming it.
 *
 * The addresses are not ours to choose.  $0801 and $2001 are where the two
 * BASICs put a program, and $2D and $82 are where they keep the end of it --
 * every tool that loads a PRG agrees on these, which is why they are written
 * out here as literals rather than derived from anything. */

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

extern "C" {
#include "prgload.h"
}

namespace {

/* $D030 as a MEGA65 at its BASIC prompt really reads: PAL, ROMC and CROM9.
 * Measured on the machine rather than assumed. */
constexpr uint8_t MEGA65_D030 = 0x64;
/* No C65 ROM mapped, which is what C64 mode means. */
constexpr uint8_t C64_D030 = 0x00;

/* A program's first bytes: the load address, then whatever it is. */
std::vector<uint8_t> program(uint16_t load, size_t payload = 16) {
    std::vector<uint8_t> out{static_cast<uint8_t>(load), static_cast<uint8_t>(load >> 8)};
    out.resize(2 + payload, 0xAA);
    return out;
}

PrgPlan planned(const std::vector<uint8_t>& prg, uint8_t d030, PrgVerdict expected = PrgOk) {
    PrgPlan out{};
    CHECK(prg_plan(prg.data(), prg.size(), d030, &out) == expected);
    return out;
}

}  // namespace

TEST_SUITE("prgload") {

TEST_CASE("the load address is the first two bytes, little endian") {
    const PrgPlan c64 = planned(program(0x0801), C64_D030);
    CHECK(c64.load == 0x0801);
    const PrgPlan m65 = planned(program(0x2001), MEGA65_D030);
    CHECK(m65.load == 0x2001);
}

/* One past the last byte, which is what BASIC's pointer holds -- and the two
 * bytes of load address are not part of the program. */
TEST_CASE("the end is one past the last byte loaded") {
    const PrgPlan plan = planned(program(0x0801, 100), C64_D030);
    CHECK(plan.end == 0x0801 + 100);
}

/* Each BASIC keeps the end of the program in its own place, and writing the
 * wrong one leaves a machine that lists nothing or lists for ever. */
TEST_CASE("each mode has its own end-of-program pointer") {
    CHECK(planned(program(0x0801), C64_D030).pointer == 0x2D);
    CHECK(planned(program(0x2001), MEGA65_D030).pointer == 0x82);
}

/* The load address says which BASIC a program was saved from, and a program
 * put into the other one is not a program that machine can run. */
TEST_CASE("a program and the frozen machine have to agree on the mode") {
    PrgPlan out{};
    CHECK(prg_plan(program(0x0801).data(), 18, MEGA65_D030, &out) == PrgWrongMode);
    CHECK(prg_plan(program(0x2001).data(), 18, C64_D030, &out) == PrgWrongMode);
}

/* Any C65 ROM mapped is the machine in its own mode; none of them is C64.
 * $64 is what a MEGA65 at the prompt reads, and the other values here are the
 * same machine with a different ROM banked. */
TEST_CASE("the frozen machine's mode is read from its $D030") {
    PrgPlan out{};
    for (const uint8_t mega : {0x64, 0x68, 0x20, 0x08, 0x10, 0x80}) {
        CHECK(prg_plan(program(0x2001).data(), 18, mega, &out) == PrgOk);
        CHECK(prg_plan(program(0x0801).data(), 18, mega, &out) == PrgWrongMode);
    }
    /* Bits that say nothing about which ROM is mapped leave it a C64. */
    for (const uint8_t c64 : {0x00, 0x01, 0x04, 0x07}) {
        CHECK(prg_plan(program(0x0801).data(), 18, c64, &out) == PrgOk);
        CHECK(prg_plan(program(0x2001).data(), 18, c64, &out) == PrgWrongMode);
    }
}

/* Everything else is refused rather than guessed at: a program saved from
 * somewhere else is one this cannot put back where it came from. */
TEST_CASE("a load address neither BASIC uses is refused") {
    PrgPlan out{};
    for (const uint16_t odd : {0x0000, 0x0400, 0x0800, 0x0802, 0x1001, 0x2000, 0xC000, 0xFFFF}) {
        CHECK(prg_plan(program(odd).data(), 18, C64_D030, &out) == PrgUnknownAddress);
        CHECK(prg_plan(program(odd).data(), 18, MEGA65_D030, &out) == PrgUnknownAddress);
    }
}

TEST_CASE("a file too short to be a program is refused") {
    PrgPlan out{};
    const std::vector<uint8_t> prg = program(0x0801);
    CHECK(prg_plan(prg.data(), 0, C64_D030, &out) == PrgTooShort);
    CHECK(prg_plan(prg.data(), 1, C64_D030, &out) == PrgTooShort);
    /* A load address and nothing after it: there is no program in it. */
    CHECK(prg_plan(prg.data(), 2, C64_D030, &out) == PrgTooShort);
    CHECK(prg_plan(prg.data(), 3, C64_D030, &out) == PrgOk);
}

/* The 16-bit space is what a program lands in, so one that would run off the
 * top of it is refused before a byte is written rather than wrapping over
 * page zero. */
TEST_CASE("a program that would not fit under the top of memory is refused") {
    PrgPlan out{};
    const std::vector<uint8_t> prg = program(0x0801);
    /* Exactly to the last byte of the space is allowed. */
    CHECK(prg_plan(prg.data(), 2 + (0x10000 - 0x0801), C64_D030, &out) == PrgOk);
    CHECK(out.end == 0x0000); /* one past $FFFF, which is where it stops */
    CHECK(prg_plan(prg.data(), 2 + (0x10000 - 0x0801) + 1, C64_D030, &out) == PrgTooBig);
    CHECK(prg_plan(prg.data(), 0x20000, C64_D030, &out) == PrgTooBig);
}

/* Each refusal has to say something, and say something different: a table that
 * lost a line would otherwise show one reason for another, and the count alone
 * cannot see that. */
TEST_CASE("every refusal has its own words") {
    std::vector<std::string> said;
    for (const PrgVerdict verdict : {PrgTooShort, PrgUnknownAddress, PrgWrongMode, PrgTooBig}) {
        const std::string words = prg_verdict_text(verdict);
        CHECK(!words.empty());
        /* The status line is eighty columns and shares none of it here. */
        CHECK(words.size() <= 80);
        said.push_back(words);
    }
    std::sort(said.begin(), said.end());
    CHECK(std::adjacent_find(said.begin(), said.end()) == said.end());
    /* Nothing went wrong, so there is nothing to say. */
    CHECK(std::string(prg_verdict_text(PrgOk)).empty());
}

}  // TEST_SUITE
