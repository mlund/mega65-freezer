#!/usr/bin/env python3
"""Check that the freeze menu draws, and that a key redraws the field it owns.

The other emulator tests reach a tool *through* the freezer and never look at
its own screen, so nothing covered the menu itself.  This does: it boots
FREEZER, reads the screen back, and asserts the fixed fragments and the live
value fields are where they belong.  That is the whole drawing path -- the
compile-time fragment stream in freezer/menu.cpp, draw_text(), draw_rule() and
the colour fills -- so a change that quietly stops drawing something fails here.

Two runs rather than one, because the dump is written on exit and so gives one
screen per emulator.  The second presses `V`, and the check is that the video
field *changed*, not that it reached a particular value: which of PAL50/NTSC60
a cold boot starts from is the emulator's default, not ours, and a test that
hardcoded it would be pinning someone else's choice.

The screen comes from a memory dump rather than -dumpscreen, which reaches only
half of it in this mode; see test/screen.py.

    python3 test/verify_freezer_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import screen
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


def _capture(args, clone: str, tmp: str, keys: str, tag: str) -> bytes:
    """Boot FREEZER, optionally type something, and return memory at exit."""
    dump = os.path.join(tmp, f"memory-{tag}.bin")
    # AF_UNIX caps near 104 characters, so the socket cannot live under a long
    # temporary directory; /tmp keeps it short enough.
    socket_path = f"/tmp/xemu-freezer-{tag}-{os.getpid()}.sock"
    proc = subprocess.Popen(
        [
            args.emulator,
            "-headless",
            "-sleepless",
            "-fastboot",
            "-testing",
            "-model",
            "3",
            "-besure",
            "-sdimg",
            clone,
            "-uartmon",
            socket_path,
            "-prgmode",
            "64",
            "-prg",
            os.path.join(args.build, "FREEZER.M65"),
            "-dumpmem",
            dump,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(args.boot)
        if keys:
            sock = xemu_keys.socket.socket(xemu_keys.socket.AF_UNIX, xemu_keys.socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect(socket_path)
            time.sleep(0.3)
            xemu_keys.type_text(sock, keys)
            time.sleep(args.settle)
            sock.close()
        time.sleep(2)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if os.path.exists(socket_path):
            os.unlink(socket_path)

    if not os.path.exists(dump):
        sys.exit(f"emulator wrote no memory dump for {tag!r}")
    return screen.load(dump)


def _check_menu(dump: bytes) -> list[str]:
    problems = []
    for row, wanted in sorted(EXPECTED.items()):
        text = screen.text(dump, row)
        for phrase in wanted:
            if phrase not in text:
                problems.append(f"row {row} is missing {phrase!r}: {text!r}")

    for row in FULL_RULE_ROWS:
        text = screen.text(dump, row)
        if text != RULE * screen.COLUMNS:
            problems.append(f"row {row} is not a full rule: {text!r}")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--build", required=True, help="directory holding the .M65 builds")
    parser.add_argument("--boot", type=float, default=16.0)
    parser.add_argument("--settle", type=float, default=4.0, help="seconds after typing")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        clone = card.inject(args.sdimg, tmp, args.build)
        cold = _capture(args, clone, tmp, "", "cold")
        toggled = _capture(args, clone, tmp, "v", "video")

    problems = _check_menu(cold)

    before = screen.text(cold, VIDEO_ROW)[VIDEO_COLUMN:].strip()
    after = screen.text(toggled, VIDEO_ROW)[VIDEO_COLUMN:].strip()
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
    problems += [f"after V: {p}" for p in _check_menu(toggled)]

    if not problems:
        print(f"freeze menu drew, and V moved the video field {before} -> {after}")
        return 0

    sys.stdout.write("\n".join(problems) + "\nScreen was:\n" + screen.screen(toggled) + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
