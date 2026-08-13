#!/usr/bin/env python3
"""Draw SPRITED's screen and check what it put there.

The sprite editor has a screen of its own at $12000 and a charset of its own,
so what it renders is not covered by anything that watches the freezer's.  The
rows below are the whole of its layout: the title, the labels, the palette
strip, the cursor readout and the preview area, each a different drawing path.

Cells hold screen codes, as every other tool's do -- SPRITED copies CHARSET A --
so these assertions read as plain text rather than as the ASCII its old
lowercase bank would have given.

    python3 test/verify_sprited_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

# TRANS_CHARACTER and SOLID_BLOCK_CHARACTER (editor.c): the empty and set
# pixels of the canvas.
TRANSPARENT = "{E6}"
SOLID = "{E0}"

STEPS = [
    # Freeze menu -> sprite editor.  Loading it off the card takes a moment.
    ("key", "s"),
    ("expect", "SPRITE EDITOR", 40),
    # The banner.  `65` is digits rather than letters, so both halves decode.
    ("expect_row", 0, "THE MEGA65 SPRITE EDITOR"),
    # The sidebar's first line, "SPRITE 0 MONO": the sprite number and its
    # colour mode, drawn by the block that sets a colour per item.
    ("expect_row", 2, "SPRITE 0 MONO"),
    # The sprite's address readout, drawn as hex digits rather than text.
    ("expect_row", 3, "$000C000"),
    # An empty canvas row: 48 transparent cells, the width of one sprite row at
    # the default zoom, drawn as a strided fill rather than a cell at a time.
    ("expect_row", 6, TRANSPARENT * 48),
    # The toolbox glyphs, patched into the charset at $15000 after the ROM copy.
    # They are the only proof the charset pointer still points at our copy: they
    # exist nowhere in the ROM set.
    ("expect_row", 21, "{E8}{E9}{EA}{EB}{EC}{ED}{EE}{EF}{F0}{F1}{F2}{F3}"),
    ("expect_row", 22, "{F4}{F5}{F6}{F7}{F8}{F9}{FA}{FB}{FC}{FD}{FE}{FF}"),
    # The cursor readout, which is punctuation and digits rather than letters.
    ("expect_row", 24, "(0,0)"),
    # The preview area: solid blocks, so both canvas characters are covered.
    ("expect_row", 10, SOLID * 15),
]

# The help screen is a full page of text drawn in one go, so it is the densest
# check available on the drawing code -- and the only one that exercises the
# credits, which share the page.  It sits behind HELP, which the synthetic
# keyboard cannot send, so it is reachable only in a build that accepts F1 in
# its place: -DHARNESS_KEYS=ON, which CMake then signals with this flag.
HELP_STEPS = [
    ("key", "f1"),
    ("expect", "TIPS & TRICKS", 20),
    # The three column headings, which the port would move before it broke them.
    ("expect_row", 2, "FILE / TXFER"),
    ("expect_row", 2, "EDIT"),
    ("expect_row", 2, "DISPLAY"),
    # Letters, digits and spacing in one row, so a conversion applied to only
    # part of the range would show.
    ("expect_row", 7, "STORE SLOT      F11"),
    # The credits share the page; V0.10 pins the version string too.
    ("expect_row", 22, "V0.10 (C) 2021 HERNAN DI PIETRO"),
]

if __name__ == "__main__":
    steps = STEPS
    if "--harness-keys" in sys.argv:
        sys.argv.remove("--harness-keys")
        steps = [*STEPS, *HELP_STEPS]
    sys.exit(scenario.run(__doc__, "FREEZER.M65", steps))
