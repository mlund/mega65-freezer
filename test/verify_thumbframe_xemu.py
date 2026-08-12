#!/usr/bin/env python3
"""Check that the menu survives a redraw with no thumbnail frame loaded.

The frame files live in the card's root, and hyppo opens by name relative to
the current directory, so the freezer skips reading them once the disk chooser
has stepped into a subdirectory.  What it did not skip was the code that reads
the frame back out of $52000 -- tile numbers, screen pointer and the thumbnail's
own position -- which on a card with no frame files has never been written.

Both halves are needed to see it, and neither is reachable from the keyboard
alone: the frames are deleted from the cloned card, and the subdirectory flag is
set through the serial monitor rather than by navigating a directory tree that
the card would have to be built to contain.  The assertion is simply that the
menu still reads correctly afterwards -- with the frame consumed unloaded, the
labels in rows 13 to 24 are overwritten with whatever $52000 decodes to.

    python3 test/verify_thumbframe_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import elf
import fat32
import screen
import xemu
import xemu_keys

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
# UpdateThumb -- and so one that reads the frame.  `V` does not: it redraws
# only the top of the menu.
TRIGGER = "right"


def _strip_frames(image: str) -> None:
    with open(image, "r+b") as handle:
        fs = fat32.FAT32(handle, card.partition_offset(image))
        for name in FRAMES:
            fs.remove(name)
        fs.flush()


def _thumbnail_box(memory: bytes) -> tuple[int, int, int, int] | None:
    """Where the tile cells are, as (column, row, width, height)."""
    cells = [
        (column, row)
        for row in range(screen.ROWS)
        for column in range(screen.COLUMNS)
        if memory[screen.SCREEN_AT - screen.SCREEN_AT + row * screen.ROW_BYTES + column * 2 + 1]
        == TILE_HIGH_BYTE
    ]
    if not cells:
        return None
    columns = [c for c, _ in cells]
    rows = [r for _, r in cells]
    return min(columns), min(rows), max(columns) - min(columns) + 1, max(rows) - min(rows) + 1


def _check(memory: bytes) -> list[str]:
    box = _thumbnail_box(memory)
    if box is None:
        return ["no thumbnail cells on the screen at all"]
    wanted = (THUMB_AT[0], THUMB_AT[1], THUMB_WIDTH, THUMB_HEIGHT)
    if box != wanted:
        return [f"thumbnail is at {box}, expected {wanted}"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--build", required=True, help="directory holding the .M65 builds")
    parser.add_argument("--boot", type=float, default=16.0)
    parser.add_argument("--settle", type=float, default=4.0, help="seconds after typing")
    args = parser.parse_args()

    flag = elf.symbol(os.path.join(args.build, "FREEZER.M65.elf"), "in_subdirectory")
    seen = {}

    def drive(sock):
        seen["cold"] = xemu.read(sock, screen.SCREEN_AT, screen.SCREEN_BYTES)
        xemu.write(sock, flag, b"\x01")
        seen["flag"] = xemu.read(sock, flag, 1)
        xemu_keys.press(sock, xemu_keys.NAMED[TRIGGER])
        time.sleep(args.settle)
        seen["redrawn"] = xemu.read(sock, screen.SCREEN_AT, screen.SCREEN_BYTES)

    with tempfile.TemporaryDirectory() as tmp:
        clone = card.inject(args.sdimg, tmp, args.build)
        _strip_frames(clone)
        xemu.launch(
            args.emulator,
            os.path.join(args.build, "FREEZER.M65"),
            sdimg=clone,
            socket_path=f"/tmp/xemu-thumbframe-{os.getpid()}.sock",
            drive=drive,
            boot=args.boot,
        )

    problems = []
    # Without this the rest proves nothing: an ignored write would leave the
    # freezer in the root, where it reads the frame it just failed to load.
    if seen["flag"] != b"\x01":
        problems.append(f"the subdirectory flag read back as {seen['flag'].hex()}, not 01")
    problems += [f"before {TRIGGER}: {p}" for p in _check(seen["cold"])]
    problems += [f"after {TRIGGER}: {p}" for p in _check(seen["redrawn"])]

    if not problems:
        print("menu survived a redraw with no frame loaded")
        return 0

    sys.stdout.write(
        "\n".join(problems) + "\nScreen was:\n" + screen.screen(seen["redrawn"], screen.SCREEN_AT) + "\n"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
