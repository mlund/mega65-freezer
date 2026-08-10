#!/usr/bin/env python3
"""Create a disk image the way a user does, and check it landed on the card.

The deepest path through the freezer that needs no frozen program: the menu
opens the disk chooser, the chooser's "new image" entry hands over to MAKEDISK,
MAKEDISK asks for a name and then writes 1600 sectors through its own FAT32
code.  Three tools' drawing and one tool's writer in one run.

It is the only cover for two of them.  diskchooser.c draws its list with its own
loops rather than through screen.c, and MAKEDISK has no test otherwise -- its
draw_box() corners and write_text() are never seen rendering.  The file is read
back with test/fat32.py, a separate implementation of the format from the C that
wrote it, so agreement means the bytes are really there.

819200 is not ours to choose: hyppo's dos_checkimage refuses to attach a D81
that is not exactly that size, so the size is checked and not just the name.

    python3 test/verify_makedisk_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

# Unlikely to exist on any card already; scenario.run asserts that before
# creating it, so a stale file cannot let the test pass by accident.
DISK_NAME = "ZZTEST"

STEPS = [
    # The freeze menu opens the chooser.
    ("key", "0"),
    # Its three fixed entries come from the code rather than from whatever disk
    # images this card carries, so they are the portable thing to assert.  Rows
    # 23 and 24 are the standing instructions, which nothing could see until the
    # screen came from memory rather than -dumpscreen.
    ("expect_row", 0, "- NO DISK -"),
    ("expect_row", 1, '- INTERNAL 3.5" -'),
    ("expect_row", 2, "- NEW D81 DD IMAGE -"),
    ("expect_row", 23, "SELECT DISK IMAGE, THEN PRESS RETURN"),
    ("expect_row", 24, "OR PRESS RUN/STOP TO LEAVE UNCHANGED"),
    # Down to the "new image" entry and in.
    ("key", "down"),
    ("key", "down"),
    ("key", "return"),
    ("expect", "ENTER NAME FOR"),
    # Name it and let it write.  1600 sectors takes a while, so this waits for
    # the machine to say so rather than for a fixed number of seconds.
    ("type", DISK_NAME),
    ("key", "return"),
    ("expect", "CREATED DISK IMAGE", 180),
    ("expect_file", f"{DISK_NAME}.D81", 819200),
]

if __name__ == "__main__":
    sys.exit(scenario.run(__doc__, "FREEZER.M65", STEPS))
