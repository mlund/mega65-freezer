#!/usr/bin/env python3
"""Check that the menu survives a redraw with no thumbnail frame loaded.

The frame files live in the card's root and hyppo opens by name relative to the
current directory, so the freezer skips reading them once the disk chooser has
stepped into a subdirectory.  The code that reads the frame back out of $52000
-- tile numbers, screen pointer and the thumbnail's own position -- has to be
skipped with it, or it decodes bytes that were never written.

Neither half is reachable from the keyboard: the frames are deleted from the
cloned card, and the subdirectory flag is set through the serial monitor rather
than by navigating a tree the card would have to be built to contain.  The
assertion is that the menu still reads correctly afterwards -- with the frame
consumed unloaded, the labels in rows 13 to 24 are overwritten with whatever
$52000 decodes to.

    python3 test/verify_thumbframe_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import fat32
import scenario

FRAMES = ("M65THUMB.M65", "C65THUMB.M65", "C64THUMB.M65")

# The thumbnail is nine cells by six, and every cell of it carries $14 in its
# high byte -- the tile source at $50000.  Nothing else on the screen does, so
# the block's bounding box is where the freezer decided to put it.
TILE_HIGH_BYTE = 0x14
THUMB_WIDTH, THUMB_HEIGHT = 9, 6

# thumb_xoff/thumb_yoff with no frame loaded, from the -1 arm of the frame
# block: column 5, and row 13 + 2.  Read from $52020 instead -- which is what
# consuming an unloaded frame does -- they are whatever the frozen program left
# there, and the block lands somewhere else.
THUMB_AT = (5, 15)

# Cursor right moves to the next slot, one of the redraws that carries
# UpdateThumb -- and so one that reads the frame.  `V` does not: it redraws only
# the top of the menu.
TRIGGER = "right"


def _strip_frames(image: str) -> None:
    with open(image, "r+b") as handle:
        fs = fat32.FAT32(handle, card.partition_offset(image))
        for name in FRAMES:
            fs.remove(name)
        fs.flush()


def _thumbnail_box(shot) -> tuple[int, int, int, int] | None:
    """Where the tile cells are, as (column, row, width, height)."""
    cells = [
        (column, row)
        for row in range(shot.screen.rows)
        for column, high in enumerate(shot.attributes(row))
        if high == TILE_HIGH_BYTE
    ]
    if not cells:
        return None
    columns = [c for c, _ in cells]
    rows = [r for _, r in cells]
    return min(columns), min(rows), max(columns) - min(columns) + 1, max(rows) - min(rows) + 1


def _check(shot) -> list[str]:
    box = _thumbnail_box(shot)
    if box is None:
        return ["no thumbnail cells on the screen at all"]
    wanted = (THUMB_AT[0], THUMB_AT[1], THUMB_WIDTH, THUMB_HEIGHT)
    if box != wanted:
        return [f"thumbnail is at {box}, expected {wanted}"]
    return []


def main() -> int:
    problems = []
    args = scenario.add_arguments(argparse.ArgumentParser(description=__doc__)).parse_args()
    with scenario.staged_card(args) as (clone, _tmp):
        _strip_frames(clone)

        with scenario.machine_on(args, clone) as machine:
            # The banner that says the menu is up is drawn before the thumbnail
            # is, so the first screen after it is not yet one to measure.
            cold, cold_problems = machine.until_ok(_check)

            flag = machine.address("FREEZER.in_subdirectory")
            machine.write(flag, b"\x01")
            # Without this the rest proves nothing: an ignored write would
            # leave the freezer in the root, where it reads the frame it just
            # failed to load.
            read_back = machine.read(flag, 1)
            if read_back != b"\x01":
                problems.append(f"the subdirectory flag read back as {read_back.hex()}, not 01")

            machine.press(TRIGGER)
            # Waiting for the thumbnail to be where it belongs, not merely for
            # the screen to differ: a redraw part way through has already
            # changed something, and the box measured then is a half-drawn one
            # rather than a wrong one.
            redrawn, redrawn_problems = machine.until_ok(_check)

    problems += [f"before {TRIGGER}: {p}" for p in cold_problems]
    problems += [f"after {TRIGGER}: {p}" for p in redrawn_problems]

    if not problems:
        print("menu survived a redraw with no frame loaded")
        return 0

    sys.stdout.write("\n".join(problems) + "\nScreen was:\n" + redrawn.whole() + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
