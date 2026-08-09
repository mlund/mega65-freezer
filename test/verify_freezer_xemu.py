#!/usr/bin/env python3
"""Check that the freeze menu draws, and that a key redraws the field it owns.

The other emulator tests reach a tool *through* the freezer and never look at
its own screen, so nothing covered the menu itself.  This does: it boots
FREEZER, reads the screen back, and asserts the fixed fragments and the live
value fields are where they belong.  That is the whole drawing path -- the
compile-time fragment stream in freezer/menu.cpp, draw_text(), draw_rule() and
the colour fills -- so a change that quietly stops drawing something fails here.

One boot, two screens: the serial monitor reads memory while the machine runs,
so the state before the keypress needs no emulator of its own.  The check on
`V` is that the video field *changed*, not that it reached a particular value
-- which of PAL50/NTSC60 a cold boot starts from is the emulator's default, not
ours, and hardcoding it would pin someone else's choice.

Reading memory rather than -dumpscreen is not optional here: that option walks
one byte per cell and so reaches only half the rows of this screen mode.  See
test/screen.py.

    python3 test/verify_freezer_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import screen
import xemu
import xemu_keys

# The rule rows: MENU_RULE_GLYPH is $43, which screen.text() shows as {43}.
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
    # Below the half -dumpscreen could reach.  These are fixed fragments too,
    # drawn whether or not anything is frozen -- the frozen program's own
    # details go in the gaps between them and are not asserted, since there is
    # no frozen program when FREEZER is loaded straight in with -prg.
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


def _check_menu(memory: bytes) -> list[str]:
    problems = []
    for row, wanted in sorted(EXPECTED.items()):
        text = screen.text(memory, row, screen.SCREEN_AT)
        for phrase in wanted:
            if phrase not in text:
                problems.append(f"row {row} is missing {phrase!r}: {text!r}")

    for row in FULL_RULE_ROWS:
        text = screen.text(memory, row, screen.SCREEN_AT)
        if text != RULE * screen.COLUMNS:
            problems.append(f"row {row} is not a full rule: {text!r}")

    return problems


def _video(memory: bytes) -> str:
    return screen.text(memory, VIDEO_ROW, screen.SCREEN_AT)[VIDEO_COLUMN:].strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--build", required=True, help="directory holding the .M65 builds")
    parser.add_argument("--boot", type=float, default=16.0)
    parser.add_argument("--settle", type=float, default=4.0, help="seconds after typing")
    args = parser.parse_args()

    seen = {}

    def drive(sock):
        seen["cold"] = xemu.read(sock, screen.SCREEN_AT, screen.SCREEN_BYTES)
        xemu_keys.type_text(sock, "v")
        time.sleep(args.settle)
        seen["toggled"] = xemu.read(sock, screen.SCREEN_AT, screen.SCREEN_BYTES)

    with tempfile.TemporaryDirectory() as tmp:
        clone = card.inject(args.sdimg, tmp, args.build)
        xemu.launch(
            args.emulator,
            os.path.join(args.build, "FREEZER.M65"),
            sdimg=clone,
            socket_path=f"/tmp/xemu-freezer-{os.getpid()}.sock",
            drive=drive,
            boot=args.boot,
        )

    problems = _check_menu(seen["cold"])

    before, after = _video(seen["cold"]), _video(seen["toggled"])
    standards = {"PAL50", "NTSC60"}
    if before not in standards:
        problems.append(f"video field read {before!r}, expected one of {sorted(standards)}")
    if after not in standards:
        problems.append(f"video field after V read {after!r}, expected one of {sorted(standards)}")
    if before == after:
        problems.append(f"V did not change the video field, still {after!r}")

    # The redraw has to leave the rest of the menu alone, not just change its
    # own field -- draw_freeze_menu() repaints by part, and a wrong part mask
    # would show up here rather than in the field itself.
    problems += [f"after V: {p}" for p in _check_menu(seen["toggled"])]

    if not problems:
        print(f"freeze menu drew, and V moved the video field {before} -> {after}")
        return 0

    shown = screen.screen(seen["toggled"], screen.SCREEN_AT)
    sys.stdout.write("\n".join(problems) + "\nScreen was:\n" + shown + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
