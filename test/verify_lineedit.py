#!/usr/bin/env python3
"""Type into SPRITED's line editor on the host, where the edges are reachable.

The editor asks for a filename and for a yes/no answer.  Driving that through
the emulator means a key at a time and a screen read to see the result, so the
cases that actually go wrong -- backspacing an empty line, the last cell, a
control code -- are the ones least likely to get typed.  The logic is pure, so
it runs here instead: the harness compiles src/sprited/lineedit.c itself, so
this is the code the tool runs and not a model of it.

    python3 test/verify_lineedit.py --cc cc
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

RETURN, DELETE = 0x0D, 0x14


def run(binary: str, capacity: int, keys: list[int]) -> tuple[int, bool, list[int]]:
    out = subprocess.run(
        [binary, str(capacity), *[str(k) for k in keys]],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    return int(out[0]), out[1] == "1", [int(v) for v in out[2:]]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()

    failures = []

    def check(name, got, want):
        if got != want:
            failures.append(f"{name}: got {got}, wanted {want}")

    with tempfile.TemporaryDirectory() as tmp:
        binary = str(Path(tmp) / "lineedit")
        subprocess.run(
            [
                args.cc,
                "-std=c2x",
                "-Wall",
                "-Werror",
                "-o",
                binary,
                str(HERE / "lineedit_host_harness.c"),
            ],
            check=True,
        )

        text = [ord(c) for c in "HELLO"]

        # The ordinary case: five keys, then RETURN ends it.
        check("typed", run(binary, 16, [*text, RETURN]), (5, True, [*text, 0]))

        # Without RETURN the line is not finished, and the buffer is still
        # terminated -- a redraw happens between every key.
        check("unfinished", run(binary, 16, text), (5, False, [*text, 0]))

        # Backspace removes one and re-terminates.
        check("backspace", run(binary, 16, [*text, DELETE]), (4, False, [*text[:4], 0]))

        # Backspacing an empty line does nothing rather than running off the
        # front, which is the bug this shape of code usually has.
        check("backspace empty", run(binary, 16, [DELETE, DELETE, DELETE]), (0, False, [0]))
        check("backspace past empty", run(binary, 16, [ord("A"), DELETE, DELETE]), (0, False, [0]))

        # Capacity includes the terminator, so capacity 4 holds three
        # characters and the fourth key is dropped rather than overrunning.
        check("full", run(binary, 4, [ord("A"), ord("B"), ord("C")]), (3, False, [65, 66, 67, 0]))
        check(
            "overfull",
            run(binary, 4, [ord("A"), ord("B"), ord("C"), ord("D")]),
            (3, False, [65, 66, 67, 0]),
        )
        # ... and having been refused, backspace still works: the length was
        # not advanced past the end.
        check(
            "backspace after full",
            run(binary, 4, [ord("A"), ord("B"), ord("C"), ord("D"), DELETE]),
            (2, False, [65, 66, 0]),
        )

        # A capacity of one is all terminator: nothing can be typed.
        check("capacity one", run(binary, 1, [ord("A")]), (0, False, [0]))

        # Control codes are refused.  conio's cinput() with CINPUT_ACCEPT_ALL
        # takes them, which would put one in a filename.
        check("control refused", run(binary, 16, [0x00, 0x01, 0x1F, 0x7F, 0x93]), (0, False, [0]))

        # Space and the printable extremes are accepted.
        check("printable edges", run(binary, 16, [0x20, 0x7E]), (2, False, [0x20, 0x7E, 0]))

        # RETURN wins wherever it appears, and stops the line there.
        check("return first", run(binary, 16, [RETURN, ord("A")]), (0, True, [0]))

    for failure in failures:
        print(failure)
    if failures:
        print(f"{len(failures)} checks failed")
        return 1
    print("13 checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
