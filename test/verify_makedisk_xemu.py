#!/usr/bin/env python3
"""Create a disk image the way a user does, and check it landed on the card.

This is the deepest path through the freezer that does not need a frozen
program: the freeze menu opens the disk chooser, the chooser's "new image"
entry hands over to MAKEDISK, MAKEDISK asks for a name and then writes 1600
sectors through its own FAT32 code.  Three tools' drawing and one tool's
writer, in one run.

It is the only cover for two things.  diskchooser.c draws its list with its
own loops rather than through screen.c, and MAKEDISK has no test at all --
its draw_box() corners and write_text() are otherwise never seen rendering.
The created file is then read back with test/fat32.py, a separate
implementation of the format from MAKEDISK's C, so agreement means the bytes
are really there and not just what the writer believes it wrote.

819200 is not ours to choose: hyppo's dos_checkimage refuses to attach a D81
that is not exactly that size, which is why the size is asserted and not just
the file's presence.

The image is a clone, so the card passed in is never written to.

    python3 test/verify_makedisk_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import argparse
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import fat32
import screen
import xemu_keys

# The chooser's three fixed entries, which come from the code rather than from
# whatever disk images the card happens to carry -- so this does not depend on
# the contents of someone else's SD image.
CHOOSER_ENTRIES = ["- NO DISK -", '- INTERNAL 3.5" -', "- NEW D81 DD IMAGE -"]

# The two rows of standing instructions along the bottom.
INSTRUCTIONS = [
    (23, "SELECT DISK IMAGE, THEN PRESS RETURN"),
    (24, "OR PRESS RUN/STOP TO LEAVE UNCHANGED"),
]

# Rows down the list from the top to reach "- NEW D81 DD IMAGE -".
NEW_IMAGE_ROW = 2

# Unlikely to exist on any card already; the test asserts that before creating.
DISK_NAME = "ZZTEST"

# A double-density D81, the size hyppo's dos_checkimage insists on.
D81_BYTES = 819200

# Offsets in a FAT32 directory entry.
ENTRY_SIZE_AT = 28


def _run(args, clone: str, tmp: str, tag: str, drive) -> bytes:
    """Boot FREEZER, let `drive` type into it, and return memory at exit."""
    dump = os.path.join(tmp, f"memory-{tag}.bin")
    # AF_UNIX caps near 104 characters, so the socket stays in /tmp.
    socket_path = f"/tmp/xemu-makedisk-{tag}-{os.getpid()}.sock"
    proc = subprocess.Popen(
        [
            args.emulator,
            "-headless",
            "-sleepless",
            "-fastboot",
            "-testing",
            "-model",
            "3",
            "-besure",
            "-sdimg",
            clone,
            "-uartmon",
            socket_path,
            "-prgmode",
            "64",
            "-prg",
            os.path.join(args.build, "FREEZER.M65"),
            "-dumpmem",
            dump,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(args.boot)
        sock = xemu_keys.socket.socket(xemu_keys.socket.AF_UNIX, xemu_keys.socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect(socket_path)
        time.sleep(0.3)
        drive(sock)
        sock.close()
        time.sleep(3)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if os.path.exists(socket_path):
            os.unlink(socket_path)

    if not os.path.exists(dump):
        sys.exit(f"emulator wrote no memory dump for {tag!r}")
    return screen.load(dump)


def _entry_size(image: str, name: str) -> int | None:
    """The size FAT32 records for `name`, read back independently of MAKEDISK."""
    with open(image, "rb") as handle:
        fs = fat32.FAT32(handle, card.partition_offset(image))
        at, _ = fs.find(fat32.short_name(name))
        if at is None:
            return None
        handle.seek(at)
        entry = handle.read(32)
    return struct.unpack_from("<I", entry, ENTRY_SIZE_AT)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--build", required=True, help="directory holding the .M65 builds")
    parser.add_argument("--boot", type=float, default=16.0)
    parser.add_argument("--create", type=float, default=90.0, help="seconds to write the image")
    args = parser.parse_args()

    filename = f"{DISK_NAME}.D81"
    problems = []

    with tempfile.TemporaryDirectory() as tmp:
        clone = card.inject(args.sdimg, tmp, args.build)

        if _entry_size(clone, filename) is not None:
            sys.exit(f"{filename} already exists on the card; the test proves nothing")

        def open_chooser(sock):
            xemu_keys.type_text(sock, "0")
            time.sleep(8)

        chooser = _run(args, clone, tmp, "chooser", open_chooser)
        listed = screen.screen(chooser)
        for entry in CHOOSER_ENTRIES:
            if entry not in listed:
                problems.append(f"disk chooser is missing {entry!r}")
        # Rows 23 and 24, which no test could see until the screen came from a
        # memory dump: the chooser writes them on every full redraw.
        for row, phrase in INSTRUCTIONS:
            if phrase not in screen.text(chooser, row):
                problems.append(f"row {row} is missing {phrase!r}: {screen.text(chooser, row)!r}")

        def make_disk(sock):
            xemu_keys.type_text(sock, "0")
            time.sleep(8)
            for _ in range(NEW_IMAGE_ROW):
                xemu_keys.press(sock, xemu_keys.NAMED["down"])
                time.sleep(1)
            xemu_keys.press(sock, xemu_keys.NAMED["return"])
            time.sleep(6)
            xemu_keys.type_text(sock, DISK_NAME)
            time.sleep(2)
            xemu_keys.press(sock, xemu_keys.NAMED["return"])
            time.sleep(args.create)

        made = _run(args, clone, tmp, "create", make_disk)
        if "CREATED DISK IMAGE" not in screen.screen(made):
            problems.append("MAKEDISK did not report creating the image")

        size = _entry_size(clone, filename)

    if size is None:
        problems.append(f"{filename} is not on the card")
    elif size != D81_BYTES:
        problems.append(f"{filename} is {size} bytes, expected {D81_BYTES}")

    if not problems:
        print(f"chooser listed its entries, and MAKEDISK wrote {filename} at {size} bytes")
        return 0

    sys.stdout.write("\n".join(problems) + "\nFinal screen was:\n" + screen.screen(made) + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
