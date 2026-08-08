"""Stage a build onto a copy of an SD image, for the emulator tests.

The freezer loads its tools from the card, so a test that means to exercise ours
has to put ours there. The image is copied first and only the copy is written
to, because the one you point at is your own.

Files go in through fat32.py, which writes the filesystem inside the image
directly: no mounting, no root, and nothing platform-specific. The partition
table is read here because the filesystem does not start at the beginning of
the image.
"""

from __future__ import annotations

import datetime
import os
import shutil
import struct
import subprocess
import sys

import fat32

# Partition types the freezer's card uses. 0x0c is FAT32 with LBA, which is what
# a MEGA65 card is formatted as; the others are accepted so an older or
# hand-made image still works.
FAT_TYPES = (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E)

MBR_TABLE = 0x1BE
SECTOR = 512


def partition_offset(image: str) -> int:
    """Byte offset of the first FAT partition, for mtools' `image@@offset`."""
    with open(image, "rb") as handle:
        mbr = handle.read(SECTOR)
    for slot in range(4):
        entry = mbr[MBR_TABLE + slot * 16 : MBR_TABLE + (slot + 1) * 16]
        kind = entry[4]
        start = struct.unpack("<I", entry[8:12])[0]
        if kind in FAT_TYPES and start:
            return start * SECTOR
    sys.exit(f"no FAT partition in {image}")


def clone(base_image: str, work_dir: str) -> str:
    """Copy the image, cheaply where the filesystem allows it.

    A card image is gigabytes, so a copy-on-write clone is the difference
    between a test that starts now and one that starts in a minute. Both flags
    fall back to a full copy when the filesystem cannot share blocks.
    """
    out = os.path.join(work_dir, "card.img")
    flag = "-c" if sys.platform == "darwin" else "--reflink=auto"
    if subprocess.run(["cp", flag, base_image, out], check=False).returncode:
        shutil.copyfile(base_image, out)
    return out


def inject(base_image: str, work_dir: str, build_dir: str, with_iomap: bool = True) -> str:
    """Clone the image and copy the build onto its filesystem."""
    card = clone(base_image, work_dir)
    with open(card, "r+b") as handle:
        fs = fat32.FAT32(handle, partition_offset(card))
        for name in sorted(os.listdir(build_dir)):
            if not name.endswith((".M65", ".BIN")):
                continue
            # IOMAP.BIN is the bit editor's names; a card without it looks
            # exactly like a database that knows nothing, which is a case worth
            # testing.
            if name.endswith(".BIN") and not with_iomap:
                continue
            built = os.path.join(build_dir, name)
            # The build's own time, so the card says when the tool was made
            # rather than when it was copied.
            when = datetime.datetime.fromtimestamp(os.path.getmtime(built))
            with open(built, "rb") as source:
                fs.write(name, source.read(), when)
        fs.flush()
    return card
