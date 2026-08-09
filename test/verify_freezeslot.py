#!/usr/bin/env python3
"""Run test_freezeslot.prg under Xemu against a cloned card.

The program does the checking itself and quits the emulator with the number of
the check that failed, so this only has to give it a card it may write to and
pass the exit status on.  A clone, because the slot round trip writes to the
image -- the one passed in is never touched.

    python3 test/verify_freezeslot.py --emulator xmega65 --sdimg card.img \\
            --prg build/src/test_freezeslot.prg
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--prg", required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        clone = card.clone(args.sdimg, tmp)
        try:
            done = subprocess.run(
                [
                    args.emulator,
                    "-headless",
                    "-sleepless",
                    "-testing",
                    "-model",
                    "3",
                    "-hyperserialascii",
                    "-besure",
                    "-sdimg",
                    clone,
                    "-prg",
                    args.prg,
                ],
                capture_output=True,
                text=True,
                timeout=args.timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            print(f"test_freezeslot did not finish within {args.timeout}s")
            return 1

    if done.returncode == 0:
        print("freeze slot round trip passed")
        return 0

    print(f"check {done.returncode} failed; see test/test_freezeslot.c")
    sys.stdout.write(done.stdout[-2000:])
    return 1


if __name__ == "__main__":
    sys.exit(main())
