#!/usr/bin/env python3
"""Draw SPRITED's screen and check what it put there.

The sprite editor had no test at all, which mattered because it is the one tool
that does not draw through src/screen.c: it uses mega65-libc's conio, its own
80x25 geometry, and its own charset.  This pins what it renders so that moving
it onto the shared drawing code can be shown not to change the picture.

Two things here are unlike the other tools' tests and are the point rather than
an accident:

Its screen is at $12000, not $B800, so the scenario is told where to read.

Its cells hold ASCII, not screen codes -- `THE` is $54 $48 $45 where the other
tools would write $14 $08 $05.  SPRITED copies an ASCII-ordered charset from
$2D800 and indexes it directly, so screen.text() renders those as {54}{48}{45}
and the assertions below match that form.  Anything that converts to screen
codes on the way in would show here immediately.

    python3 test/verify_sprited_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

# Where SPRITED keeps its screen.  Moves to $B800 when it joins screen.c.
SPRITED_SCREEN = 0x12000

# TRANS_CHARACTER and SOLID_BLOCK_CHARACTER (editor.c): the empty and set
# pixels of the canvas.
TRANSPARENT = "{E6}"
SOLID = "{E0}"

STEPS = [
    # Freeze menu -> sprite editor.  Loading it off the card takes a moment.
    ("key", "s"),
    ("expect", "{53}{50}{52}{49}{54}{45} {45}{44}{49}{54}{4F}{52}", 40),
    # The banner, as ASCII: "THE MEGA65 SPRITE EDITOR".  `65` is $36 $35, which
    # falls in the range screen.text() prints literally, so it appears as-is
    # between the bracketed letters -- a good check that both halves decode.
    ("expect_row", 0, "{4D}{45}{47}{41}65 {53}{50}{52}{49}{54}{45}"),
    # The sidebar's first line, "SPRITE 0 MONO": the sprite number and its
    # colour mode, drawn by the block that sets a colour per item.
    ("expect_row", 2, "{53}{50}{52}{49}{54}{45} 0 {4D}{4F}{4E}{4F}"),
    # The sprite's address readout, drawn as hex digits rather than text.
    ("expect_row", 3, "$000{43}000"),
    # An empty canvas row: 48 transparent cells, the width of one sprite row at
    # the default zoom.  This is the draw_fragment run in the port.
    ("expect_row", 6, TRANSPARENT * 48),
    # The toolbox glyphs, patched into the charset at $15000 after the ROM copy.
    # They are the only proof the charset pointer still points at our copy: they
    # exist nowhere in the ROM set.
    ("expect_row", 21, "{E8}{E9}{EA}{EB}{EC}{ED}{EE}{EF}{F0}{F1}{F2}{F3}"),
    ("expect_row", 22, "{F4}{F5}{F6}{F7}{F8}{F9}{FA}{FB}{FC}{FD}{FE}{FF}"),
    # The cursor readout, the one place a plain string survives unbracketed.
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
    ("expect", "{54}{49}{50}{53} & {54}{52}{49}{43}{4B}{53}", 20),
    # The three column headings, which the port would move before it broke them.
    ("expect_row", 2, "{46}{49}{4C}{45} / {54}{58}{46}{45}{52}"),
    ("expect_row", 2, "{45}{44}{49}{54}"),
    ("expect_row", 2, "{44}{49}{53}{50}{4C}{41}{59}"),
    # A row mixing bracketed letters, a digit and punctuation, so a conversion
    # applied to only part of the range would show.
    ("expect_row", 7, "{53}{54}{4F}{52}{45} {53}{4C}{4F}{54}      {46}11"),
    # The credits share the page; V0.10 pins the version string too.
    (
        "expect_row",
        22,
        "{56}0.10 ({43}) 2021 {48}{45}{52}{4E}{41}{4E} {44}{49} {50}{49}{45}{54}{52}{4F}",
    ),
]

if __name__ == "__main__":
    steps = STEPS
    if "--harness-keys" in sys.argv:
        sys.argv.remove("--harness-keys")
        steps = [*STEPS, *HELP_STEPS]
    sys.exit(scenario.run(__doc__, "FREEZER.M65", steps, screen_at=SPRITED_SCREEN))
