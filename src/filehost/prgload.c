/* The rules a program has to pass before any of it reaches the freeze slot. */

#include "prgload.h"

/* Where each BASIC keeps the end of the program.  Writing the other one's
 * leaves a machine that lists nothing, or lists for ever. */
static constexpr uint16_t PRG_POINTER_C64 = 0x2d;
static constexpr uint16_t PRG_POINTER_MEGA65 = 0x82;

/* $D030 maps the C65 ROM in four places, and any of them mapped is the machine
 * running its own BASIC rather than the C64's.  Tested as bits and not as a
 * value: $64 is what a MEGA65 at its prompt reads -- PAL, ROMC and CROM9,
 * measured on the machine -- but which ROM is banked at the moment of the
 * freeze is not something a program has to leave alone. */
static constexpr uint8_t D030_ROM8 = 0x08;
static constexpr uint8_t D030_ROMA = 0x10;
static constexpr uint8_t D030_ROMC = 0x20;
static constexpr uint8_t D030_CROM9 = 0x40;
static constexpr uint8_t D030_ROME = 0x80;
static constexpr uint8_t D030_C65_ROM =
    D030_ROM8 | D030_ROMA | D030_ROMC | D030_CROM9 | D030_ROME;

/* One past the last address a program may occupy. */
static constexpr uint32_t MEMORY_TOP = 0x10000;

enum PrgVerdict prg_plan(
    const uint8_t* head, uint32_t size, uint8_t frozen_d030, struct PrgPlan* out) {
    /* Two bytes of load address and at least one of program: a file that is
     * only an address says where nothing goes. */
    if (size < 3) {
        return PrgTooShort;
    }

    const uint16_t load = (uint16_t)(head[0] | ((uint16_t)head[1] << 8));
    const bool mega65 = (frozen_d030 & D030_C65_ROM) != 0;

    if (load != PRG_START_C64 && load != PRG_START_MEGA65) {
        return PrgUnknownAddress;
    }
    if ((load == PRG_START_MEGA65) != mega65) {
        return PrgWrongMode;
    }

    const uint32_t payload = size - 2;
    if (load + payload > MEMORY_TOP) {
        return PrgTooBig;
    }

    out->load = load;
    /* Deliberately allowed to be 0: a program filling memory to $FFFF ends one
     * past it, and that is what BASIC's pointer holds. */
    out->end = (uint16_t)(load + payload);
    out->pointer = mega65 ? PRG_POINTER_MEGA65 : PRG_POINTER_C64;
    return PrgOk;
}

const char* prg_verdict_text(enum PrgVerdict verdict) {
    static const char* const WHY[] = {
        "",
        "THAT FILE IS TOO SHORT TO BE A PROGRAM",
        "IT WAS NOT SAVED FROM EITHER BASIC",
        "IT WAS SAVED FROM THE OTHER MODE, WHICH IS NOT THE ONE FROZEN HERE",
        "IT WOULD NOT FIT UNDER THE TOP OF MEMORY",
    };
    static_assert(sizeof WHY / sizeof *WHY == PrgTooBig + 1,
        "a verdict with no words would index past this table");
    return verdict < sizeof WHY / sizeof *WHY ? WHY[verdict] : "";
}
