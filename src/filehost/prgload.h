#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Whether a program may be put into the frozen machine, and where it goes.
 *
 * A .prg needs no file on the card: it is memory, and the freeze slot holds
 * the frozen machine's.  Which BASIC it was saved from, whether that is the
 * one running, and which pointer learns where it ends are all arithmetic, so
 * they are settled here, away from the hardware, and tested on the host.
 *
 * Nothing here writes.  PrgOk is a plan; anything else is a reason to show and
 * nothing to undo, which is why it is all settled before the first byte. */

/* Where each BASIC starts a program, which is also how a file says which one it
 * was saved from.  Not ours to choose: etherload, m65, matrix65 and Xemu all
 * read a PRG's first two bytes this way. */
constexpr uint16_t PRG_START_C64 = 0x0801;
constexpr uint16_t PRG_START_MEGA65 = 0x2001;

enum PrgVerdict : uint8_t {
    PrgOk,
    PrgTooShort,       /* a load address and nothing to load */
    PrgUnknownAddress, /* saved from somewhere neither BASIC starts */
    PrgWrongMode,      /* the frozen machine is running the other BASIC */
    PrgTooBig,         /* would run off the top of the 16-bit space */
};

struct PrgPlan {
    uint16_t load;    /* where the first byte goes */
    uint16_t end;     /* one past the last, which is what the pointer holds */
    uint16_t pointer; /* the zero-page address that keeps `end` */
};

/* `head` is the file's first two bytes and `size` its whole length, so the
 * program itself is two bytes shorter.  `frozen_d030` is the frozen machine's
 * own $D030, which says which ROM it has mapped and so which BASIC is running:
 * freeze_io_peek(0x3030) reads it. */
[[nodiscard]] enum PrgVerdict prg_plan(
    const uint8_t* head, uint32_t size, uint8_t frozen_d030, struct PrgPlan* out);

/* Why it was refused, in words for the status line, and "" for PrgOk.  Beside
 * the rules rather than in the caller: a verdict and what it means to a reader
 * are one thing, and split apart nothing checks that each has the other. */
[[nodiscard]] const char* prg_verdict_text(enum PrgVerdict verdict);
