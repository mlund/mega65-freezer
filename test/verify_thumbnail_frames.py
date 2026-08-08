#!/usr/bin/env python3
"""Check the thumbnail frames the freezer loads off the card.

The hashes pin the exact bytes the freezer is drawn against.  Nothing else
checks them: a frame that decoded differently would still load and still draw,
just wrongly, and only on a real screen.

The layout checks cover what freezer/main.c reaches by absolute offset, where a
moved field would be read as valid data rather than rejected.

    python3 test/verify_thumbnail_frames.py
"""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

# Of the frames as the freezer expects them, at the 5,1 tile origin it assumes.
EXPECTED = {
    "M65THUMB.M65": ("m65", "91552af644135e5d980ebd7c9a343bb39f87584445765cf2371d4a7ab8533418"),
    "C65THUMB.M65": ("c65", "4e46540390b6dbd182b069396424bd527ec526b6e086cf530c27349e295311e0"),
    "C64THUMB.M65": ("c64", "0065bcd453e12e0d4005b1287ae03c5437db3551a7d6849daa6e9f965143cf21"),
}

# freezer/main.c reads the tile data from $52340 and finds the screen through the
# size field at $5203D, both counted from the file loaded at $52000.
TILES_OFFSET = 0x340
SIZE_FIELD = 0x3D


def check_layout(name: str, data: bytes, problems: list[str]) -> None:
    if data[:16] != b"MEGA65 TILESET00":
        problems.append(f"{name}: not a tileset")
        return
    tiles = data[16] | (data[17] << 8)
    if TILES_OFFSET != 64 + 3 * 256:
        problems.append(f"{name}: tile data no longer starts where the freezer looks")
    size = data[SIZE_FIELD] | (data[SIZE_FIELD + 1] << 8) | (data[SIZE_FIELD + 2] << 16)
    if size != TILES_OFFSET + tiles * 64:
        problems.append(f"{name}: size field {size} does not point past the tiles")
    # The freezer skips the screen header by adding 0x40 to the size field.
    if data[size : size + 15] != b"MEGA65 SCREEN00":
        problems.append(f"{name}: no screen section where the size field points")
    if data[size + 16] != 32:
        problems.append(f"{name}: screen is not the 32 tiles a row the freezer assumes")
    if data[size + 17] != 12:
        problems.append(f"{name}: screen is not the 12 rows the freezer copies")


def main() -> int:
    problems: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, (stem, digest) in EXPECTED.items():
            out = pathlib.Path(tmp) / name
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "gen_thumbnail_frames.py"),
                    str(ROOT / "assets" / f"thumbnail-surround-{stem}.png"),
                    "--xoff",
                    "5",
                    "--yoff",
                    "1",
                    "-o",
                    str(out),
                ],
                check=True,
                capture_output=True,
            )
            data = out.read_bytes()
            got = hashlib.sha256(data).hexdigest()
            if got != digest:
                problems.append(f"{name}: {got} is not the expected {digest}")
            check_layout(name, data, problems)

    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1
    print(f"{len(EXPECTED)} thumbnail frames match byte for byte")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
