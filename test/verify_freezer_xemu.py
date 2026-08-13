#!/usr/bin/env python3
"""Check that the freeze menu draws, and that a key redraws the field it owns.

The other emulator tests reach a tool *through* the freezer and never look at
its own screen.  This boots FREEZER, reads the screen back and asserts the fixed
fragments and the live value fields are where they belong -- the whole drawing
path, so a change that quietly stops drawing something fails here.

One boot, two screens: the serial monitor reads memory while the machine runs,
so the state before the keypress needs no emulator of its own.  The check on `V`
is that the video field *changed*, not that it reached a particular value --
which of PAL50/NTSC60 a cold boot starts from is the emulator's choice, and
hardcoding it would pin someone else's.

    python3 test/verify_freezer_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

# The rule rows: MENU_RULE_GLYPH is $43, which a snapshot shows as {43}.
RULE = "{43}"

# Row -> text that must appear in it.  Each entry is a different drawing path:
# rows 0/1 and 3 and 9-11 are fixed fragments, 5-7 are labels with live values
# written over them, and the labels carry the shifted-letter key hints that the
# compile-time conversion in menu.cpp has to preserve.
EXPECTED = {
    0: ["MEGA65 FREEZE MENU"],
    1: ["MUSEUM OF ELECTRONIC GAMES"],
    3: ["F3-RESUME", "F5-RESET", "HELP-MEGAINFO"],
    5: ["(C)PU MODE:", "(J)OY SWAP:"],
    6: ["CPU (F)REQ:", "CAR(T) ENABLE:"],
    7: ["C(R)T EMU:", "(V)IDEO:"],
    9: ["M - MONITOR", "L - LOAD ROM/CHAR"],
    10: ["A - AUDIO & VOLUME"],
    11: ["S - SPRITE EDITOR"],
    # Below the half -dumpscreen could reach.  Fixed fragments too, drawn
    # whether or not anything is frozen -- the frozen program's own details go
    # in the gaps between them and are not asserted, since there is no frozen
    # program when FREEZER is loaded straight in with -prg.
    15: ["ROM:"],
    16: ["TASK ID:"],
    19: ["(0) INTERNAL DRIVE:"],
    20: ["(8) UNIT #"],
    22: ["(1) EXTERNAL 1565:"],
    23: ["(9) UNIT #"],
}
FULL_RULE_ROWS = (2, 4, 8)

# Column 32 onwards on row 7, the field `V` rewrites.
VIDEO_ROW = 7
VIDEO_COLUMN = 32
STANDARDS = {"PAL50", "NTSC60"}


def _check_menu(shot) -> list[str]:
    problems = []
    for row, wanted in sorted(EXPECTED.items()):
        text = shot.text(row)
        for phrase in wanted:
            if phrase not in text:
                problems.append(f"row {row} is missing {phrase!r}: {text!r}")

    for row in FULL_RULE_ROWS:
        text = shot.text(row)
        if text != RULE * shot.columns:
            problems.append(f"row {row} is not a full rule: {text!r}")

    return problems


def _video(shot) -> str:
    return shot.text(VIDEO_ROW)[VIDEO_COLUMN:].strip()


def _drawn(shot) -> list[str]:
    """Everything the menu must show once it has finished painting."""
    problems = _check_menu(shot)
    if _video(shot) not in STANDARDS:
        problems.append(f"video field reads {_video(shot)!r}, wanted one of {sorted(STANDARDS)}")
    return problems


def main() -> int:
    with scenario.staged_machine(__doc__) as (_args, machine):
        # The banner that says the menu is up is only its first row, so the
        # rest of it -- and the video field this test reads -- may not be
        # painted yet.  Waiting for what is about to be asserted cannot measure
        # a half-drawn screen; if it never arrives, the checks below say what
        # was missing rather than only that something was.
        cold, problems = machine.until_ok(_drawn)
        before = _video(cold)

        machine.press("v")
        # The whole menu again, not just the field: `V` repaints by part, and a
        # wrong part mask shows up elsewhere on the screen rather than here.
        toggled, after_problems = machine.until_ok(
            lambda s: _drawn(s) + ([] if _video(s) != before else ["V did not move the field"])
        )

    problems += [f"after V: {p}" for p in after_problems]
    after = _video(toggled)
    if not problems:
        print(f"freeze menu drew, and V moved the video field {before} -> {after}")
        return 0

    sys.stdout.write("\n".join(problems) + "\nScreen was:\n" + toggled.whole() + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
