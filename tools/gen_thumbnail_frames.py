#!/usr/bin/env python3
"""Turn a thumbnail surround PNG into the tileset the freezer loads.

The freezer draws a frame around the frozen program's thumbnail and picks the
frame by machine type, loading `M65THUMB.M65`, `C65THUMB.M65` or `C64THUMB.M65`
off the card at run time (freezer/main.c).  Each is a MEGA65 tileset: a palette,
the unique 8x8 tiles of the image, and a screen that indexes them.

The PNGs are 8-bit and not interlaced, so decoding them is a zlib inflate and an
unfilter -- small enough to do here, and it keeps the build free of a C compiler
and an image library for three files that never change.

test/verify_thumbnail_frames.py pins the output, byte for byte.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

# The frame is drawn at a fixed size, so anything else is a mistake in the PNG
# rather than something to scale.
IMAGE_WIDTH = 152
IMAGE_HEIGHT = 96

# The freezer computes pixel addresses assuming rows are 32 tiles wide, so the
# screen is padded out to that whatever the image measures.
SCREEN_TILES = 32

# The C64 colours the freeze menu uses, as RGB triples with a spare byte.  The
# spare is dropped here; the nybble swap below is what the hardware wants.
C64_PALETTE = (
    (0x00, 0x00, 0x00), (0xFF, 0xFF, 0xFF), (0xBA, 0x13, 0x62), (0x66, 0xAD, 0xFF),
    (0xBB, 0xF3, 0x8B), (0x55, 0xEC, 0x85), (0xD1, 0xE0, 0x79), (0xAE, 0x5F, 0xC7),
    (0x9B, 0x47, 0x81), (0x87, 0x37, 0x00), (0xDD, 0x39, 0x78), (0xB5, 0xB5, 0xB5),
    (0xB8, 0xB8, 0xB8), (0x0B, 0x4F, 0xCA), (0xAA, 0xD9, 0xFE), (0x8B, 0x8B, 0x8B),
)  # fmt: skip


def nybl_swap(value: int) -> int:
    return ((value & 0x0F) << 4) | ((value & 0xF0) >> 4)


def build_palette() -> list[tuple[int, int, int]]:
    """The menu's palette: 16 C64 colours, then an RRRGGGBB cube.

    The cube is short of greens and blues, which the C64 entries make up for.
    The C64 entries are stored nybble-swapped, the order the palette registers
    read them in.
    """
    colours = [tuple(nybl_swap(channel) for channel in entry) for entry in C64_PALETTE]
    for index in range(16, 256):
        colours.append((index & 0xE0, (index << 3) & 0xE0, (index << 6) & 0xC0))
    return colours


def match_colour(palette: list[tuple[int, int, int]], rgb: tuple[int, int, int]) -> int:
    """Nearest palette entry, weighted towards preserving brightness.

    Entry 0 is skipped so a colour never lands on it: index 0 in a screen cell
    means "nothing here", and a pixel that matched it would punch a hole.
    """
    red, green, blue = rgb
    best, best_error = -1, None
    for index in range(1, len(palette)):
        have = palette[index]
        if have == rgb:
            # $26 is the blue the surrounds use as background, and the menu
            # wants the C64 blue there instead.
            return 0x06 if index == 0x26 else index
        error = 3 * (red - have[0]) ** 2 + 6 * (green - have[1]) ** 2 + (blue - have[2]) ** 2
        if best_error is None or error < best_error:
            best, best_error = index, error
    return best


def read_png(path: pathlib.Path) -> tuple[int, int, int, bytes]:
    """Decode to 8-bit RGB or RGBA rows, returning (width, height, channels, pixels).

    Only what the surrounds actually are: 8 bits per channel and no interlace.
    A palette image is expanded, so tiles only ever see RGB.
    """
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG")

    width = height = depth = colour_type = interlace = 0
    palette = b""
    compressed = bytearray()
    at = 8
    while at < len(data):
        (length,) = struct.unpack(">I", data[at : at + 4])
        kind = data[at + 4 : at + 8]
        body = data[at + 8 : at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        at += 12 + length

    if depth != 8 or interlace:
        sys.exit(f"{path}: need 8 bits per channel and no interlace")
    if colour_type not in (2, 3, 6):
        sys.exit(f"{path}: colour type {colour_type} is not supported")
    if (width, height) != (IMAGE_WIDTH, IMAGE_HEIGHT):
        sys.exit(f"{path}: must be {IMAGE_WIDTH}x{IMAGE_HEIGHT}, not {width}x{height}")

    stored = {2: 3, 3: 1, 6: 4}[colour_type]
    raw = zlib.decompress(bytes(compressed))
    pixels = unfilter(raw, width, height, stored)

    if colour_type == 3:
        # Expanded to RGB, so the tile reader treats it like any other image.
        expanded = bytearray()
        for index in pixels:
            expanded += palette[index * 3 : index * 3 + 3]
        return width, height, 3, bytes(expanded)
    return width, height, stored, pixels


def unfilter(raw: bytes, width: int, height: int, channels: int) -> bytes:
    """Undo the per-row filters PNG stores ahead of each scanline."""
    stride = width * channels
    out = bytearray()
    previous = bytearray(stride)
    at = 0
    for _ in range(height):
        method = raw[at]
        line = bytearray(raw[at + 1 : at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upleft = previous[i - channels] if i >= channels else 0
            if method == 0:
                pass
            elif method == 1:
                line[i] = (line[i] + left) & 0xFF
            elif method == 2:
                line[i] = (line[i] + up) & 0xFF
            elif method == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif method == 4:
                line[i] = (line[i] + paeth(left, up, upleft)) & 0xFF
            else:
                sys.exit(f"unknown PNG row filter {method}")
        out += line
        previous = line
    return bytes(out)


def paeth(left: int, up: int, upleft: int) -> int:
    estimate = left + up - upleft
    da, db, dc = abs(estimate - left), abs(estimate - up), abs(estimate - upleft)
    if da <= db and da <= dc:
        return left
    return up if db <= dc else upleft


def to_tiles(
    width: int, height: int, channels: int, pixels: bytes, palette: list
) -> tuple[list[bytes], list[int | None]]:
    """Split the image into unique 8x8 tiles and the screen that indexes them.

    An entirely transparent block has no tile at all and comes back as None --
    that is how the surround leaves the middle open for the thumbnail itself.
    It is not the same as tile index 0, which is an ordinary tile that happens
    to be the first one seen.

    Tiles are shared only on an exact match.  A flipped match would have to say
    so in the screen cell's top bits, and the freezer adds its tile offset to
    that cell whole (freezer/main.c), so any flag there would be corrupted.
    """
    tiles: list[bytes] = []
    screen: list[int | None] = []
    cache: dict[bytes, int] = {}

    for top in range(0, height, 8):
        for left in range(0, width, 8):
            block = bytearray(64)
            opaque = False
            for y in range(8):
                row = (top + y) * width * channels
                for x in range(8):
                    at = row + (left + x) * channels
                    alpha = pixels[at + 3] if channels == 4 else 0xFF
                    if alpha:
                        opaque = True
                        block[y * 8 + x] = match_colour(
                            palette, (pixels[at], pixels[at + 1], pixels[at + 2])
                        )
            if not opaque:
                screen.append(None)
                continue
            key = bytes(block)
            if key not in cache:
                cache[key] = len(tiles)
                tiles.append(key)
            screen.append(cache[key])
    return tiles, screen


def build(png: pathlib.Path, xoff: int, yoff: int) -> bytes:
    palette = build_palette()
    width, height, channels, pixels = read_png(png)
    tiles, screen = to_tiles(width, height, channels, pixels, palette)
    across, down = width // 8, height // 8

    out = bytearray()

    # Tileset: a 64-byte header, the palette split into three planes, then the
    # tiles.  The count byte is deliberately the low byte of 256, as written.
    header = bytearray(64)
    header[0:16] = b"MEGA65 TILESET00"
    header[16] = len(tiles) & 0xFF
    header[17] = (len(tiles) >> 8) & 0xFF
    header[18] = len(palette) & 0xFF
    header[32] = xoff | (yoff << 4)
    size = 64 + 3 * 256 + len(tiles) * 64
    header[61], header[62], header[63] = (
        size & 0xFF,
        (size >> 8) & 0xFF,
        (size >> 16) & 0xFF,
    )
    out += header
    for plane in range(3):
        out += bytes(nybl_swap(colour[plane]) for colour in palette)
    for tile in tiles:
        out += tile

    # Screen: the tile index per cell, then the colour per cell, each row padded
    # out to the 32 the freezer assumes.
    header = bytearray(64)
    header[0:15] = b"MEGA65 SCREEN00"
    header[15] = 0
    header[16] = SCREEN_TILES
    header[17] = down
    size = 64 + 2 * (2 * across * down)
    header[61], header[62], header[63] = (
        size & 0xFF,
        (size >> 8) & 0xFF,
        (size >> 16) & 0xFF,
    )
    out += header

    padding = bytes(2 * (SCREEN_TILES - across))
    for row in range(down):
        for cell in screen[row * across : (row + 1) * across]:
            index = 0 if cell is None else cell
            out += bytes((index & 0xFF, (index >> 8) & 0xFF))
        out += padding
    for row in range(down):
        for cell in screen[row * across : (row + 1) * across]:
            # Foreground only, and an empty cell carries no colour at all.
            out += b"\x00\x00" if cell is None else b"\x00\xff"
        out += padding

    # An all-zero header ends the file.
    out += bytes(64)
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("png", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path, required=True)
    parser.add_argument("--xoff", type=int, default=5, help="thumbnail origin, in tiles")
    parser.add_argument("--yoff", type=int, default=1)
    args = parser.parse_args()

    if not 0 <= args.xoff <= 9 or not 0 <= args.yoff <= 5:
        sys.exit("xoff must be 0-9 and yoff 0-5: both share one byte of the header")

    data = build(args.png, args.xoff, args.yoff)
    args.output.write_bytes(data)
    print(f"{args.png.name} -> {args.output} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
