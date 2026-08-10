#!/usr/bin/env python3
"""Reach AUDIOMIX's advanced view and check the cursor moves.

A bare `-prg AUDIOMIX.M65` never starts: dropped into memory cold, the BASIC
command sits unexecuted at the prompt.  So this goes in through the freezer's
own launch path, which is also why it needs an SD image -- the freezer loads its
tools from the card.

`A` from the freeze menu opens the simple dB-bar view, a second `A` switches to
the advanced hex matrix, and cursor-right moves the column cursor.  The row-3
readout is deterministic where the coefficients themselves are not:
select_row/select_column reset on every entry to advanced mode, so it reads "00"
on arrival and "10" after one cursor-right, whatever the hardware registers held
from a previous run.

    python3 test/verify_audiomix_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

STEPS = [
    # Freeze menu -> AUDIOMIX.  The banner is the first thing that says the
    # tool loaded off the card and started.
    ("key", "a"),
    ("expect", "MEGA65 AUDIO MIXER MENU", 30),
    # Simple view -> advanced.  Its column header appears only here, so it
    # distinguishes the two views rather than merely proving AUDIOMIX is up.
    ("key", "a"),
    ("expect_row", 3, "LFT RGT PH1 PH2 BTL BTR HDL HDR"),
    # The readout is column then row, so it is "00" before and "10" after.
    ("expect_at", 3, 2, "00"),
    ("key", "right"),
    ("expect_at", 3, 2, "10"),
]

if __name__ == "__main__":
    sys.exit(scenario.run(__doc__, "FREEZER.M65", STEPS))
