#!/usr/bin/env python3
"""Check the freezer can launch a tool off the card at all.

`-prg` drops a program into memory cold, which is enough for the freezer itself
but not for the tools it loads: they arrive through its own launch path --
region list, find, load, run -- which nothing else exercises.  So this boots the
freezer and asks it for the monitor, and looks for something only the monitor
draws.

The register header rather than a title: a title could survive a launch that
loaded the wrong thing or stopped halfway.

    python3 test/verify_launch.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

STEPS = [
    ("key", "m"),
    ("expect", "PC   IRQ  NMI", 30),
]

if __name__ == "__main__":
    sys.exit(scenario.run(__doc__, "FREEZER.M65", STEPS))
