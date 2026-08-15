#!/usr/bin/env python3
"""Browse the FileHost catalogue, fetch with no network, and attach an image.

The whole of the tool that needs no network: the freeze menu launches it, it
reads a catalogue off the card, renders records the machine has never seen, and
attaches the image one of them names.  The catalogue is written by
test/filehost_catalog.py, which shares no code with the decoder under test.

Three things here are only visible through a screen dump.  Titles arrive as
mixed-case ASCII and the screen is uppercase-only PETSCII, so "Attack of the
Robots" rendering as letters at all is the conversion working.  Choosing a
program rather than a disk image has to say so instead of failing quietly.  And
the mount is only real if the freeze menu's own drive row shows it afterwards --
that row is drawn from the frozen process descriptor, which is what the unfreeze
path reattaches from, so it is the thing that proves the attach will survive a
resume rather than merely having happened.

819200 is not ours to choose: hyppo's dos_checkimage refuses to attach a D81
that is not exactly that size.

    python3 test/verify_filehost_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import filehost_catalog as fhc
import scenario

# The name the browser derives from the first record's path, and so the file
# the attach will look for.  Derived rather than written out: the machine works
# it out from the path, and the point of staging it under the same name is that
# the two derivations agree.
IMAGE_PATH = "files/t/Attack_of_the_Robots_x1.d81"
IMAGE = fhc.catalog_short_name(IMAGE_PATH, fhc.D81)

CATALOG = fhc.catalogue(
    [
        # Mixed case, so the render has to fold it.
        fhc.record("Attack of the Robots", "Somebody", IMAGE_PATH, fhc.D81, 819200),
        fhc.record("Hello World", "Nobody", "files/h/hello.prg", fhc.PRG, 4096),
    ]
)

# The first list row, and the two rows the fixture fills.  LIST_TOP_Y in
# src/filehost/browser.h.
FIRST = 3

STEPS = [
    ("key", "d"),
    ("expect_row", FIRST, "ATTACK OF THE ROBOTS", 30),
    ("expect_row", FIRST + 1, "HELLO WORLD"),
    ("expect_row", FIRST, "SOMEBODY"),
    # The columns, so a heading and its values cannot drift apart unnoticed.
    ("expect_at", FIRST, 58, "D81"),
    ("expect_at", FIRST + 1, 58, "PRG"),
    ("expect_at", FIRST, 62, "800KB"),
    # Down onto the program and in.  The refusal is also what proves the
    # selection moved, which no screen dump shows directly -- the highlight is
    # a colour, and colour RAM is not in the dump.
    ("key", "down"),
    ("key", "return"),
    ("expect", "ONLY DISK IMAGES"),
    # A fetch with no network at all.  Xemu has no ethernet, so nothing answers
    # the lease and the tool has to say so and put the card's catalogue back
    # rather than sit waiting -- the one part of the network path that can be
    # tested without a wire, and the part a user meets most often.
    ("key", "f"),
    ("expect", "NO ADDRESS: NOTHING ANSWERED ON THE NETWORK", 30),
    # Back to the top.  Not cursor-up: that is SHIFT plus cursor-down on this
    # keyboard, and the modifier does not reach the key queue.
    ("key", ","),
    ("key", "return"),
    ("expect", f"ATTACHED {IMAGE}", 30),
    # Out, and the freeze menu's drive row has to agree.
    ("key", "stop"),
    ("expect", "MEGA65 FREEZE MENU", 30),
    ("expect_row", 21, IMAGE),
]

if __name__ == "__main__":
    sys.exit(
        scenario.run(
            __doc__,
            "FREEZER.M65",
            STEPS,
            extras={"CATALOG.M65": CATALOG, IMAGE: bytes(819200)},
        )
    )
