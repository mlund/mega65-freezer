#!/usr/bin/env python3
"""Check the bit editor's table against its own structure, on the host.

src/monitor/bitedit_table.c reaches its database only through iomap_byte(), so
the host can hand it IOMAP.M65 in memory and run the whole decoder with no
MEGA65 and no emulator.  That matters because the record stream is variable
length -- a length byte, three bytes of packed 3-bit indices, names terminated
by bit 7 and two-byte offsets into a shared text pool -- and an off-by-one there
shifts every name onto the wrong bit, which on hardware looks like a database
mistake rather than a decoder one.

The invariants below need only the committed src/monitor/iomap.bin, so they run
in a plain checkout.  Pass --iomap to additionally diff the decoded names
against a fresh parse of mega65-core's iomap.txt, which is the only check that
can catch the generator and the decoder agreeing on something wrong.

Run via CTest, or by hand::

    python3 test/verify_bitedit.py --cc cc
"""

from __future__ import annotations

import argparse
import itertools
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

# Mirrors bitedit_table.h.  Duplicated rather than parsed out so that a column
# moving in the C without anyone meaning it fails here.
ROW_WIDTH = 78
ADDRESS_WIDTH = 8
NAME_COLUMN = 10
NAME_WIDTH = 8
VALUE_COLUMN = 19
BIT_COLUMN = 23
BIT_STRIDE = 7
BIT_WIDTH = 6
GLYPH_BIT = 0x51
CELL_VALUE = 8

# Screen codes a name may contain: A-Z, the digits, and the hyphen in VIC-IV.
NAME_CODES = set(range(0x01, 0x1B)) | set(range(0x30, 0x3A)) | {0x2D}


DATABASE = ROOT / "src" / "monitor" / "iomap.bin"


def build_harness(workdir: Path, host_cc: str) -> Path:
    """Compile bitedit_table.c plus the host driver with the host compiler."""
    binary = workdir / "bitedit_harness"
    subprocess.run(
        [
            host_cc,
            "-std=c2x",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-char-subscripts",
            "-I",
            str(ROOT / "src"),
            "-I",
            str(ROOT / "src" / "monitor"),
            "-o",
            str(binary),
            str(HERE / "bitedit_host_harness.c"),
            str(ROOT / "src" / "format.c"),
        ],
        check=True,
    )
    return binary


def run(binary: Path, commands: list[str]) -> list[str]:
    out = subprocess.run(
        [str(binary), str(DATABASE)],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    return out.stdout.splitlines()


def decode(hex_row: str) -> list[int]:
    return list(bytes.fromhex(hex_row))


def text(codes: list[int]) -> str:
    """Screen codes back to something readable, for failure messages."""
    return "".join(chr(c + 0x40) if 1 <= c <= 26 else chr(c) if c >= 0x20 else "?" for c in codes)


def check_walk(binary: Path, failures: list[str]) -> list[int]:
    """Every record's length must land exactly on the next record's address."""
    lines = run(binary, ["walk"])
    addresses = []
    for line in lines[:-1]:
        address, length = line.split()
        addresses.append(int(address, 16))
        if not 4 <= int(length) <= 0xFF:
            failures.append(f"${address} record length {length} is implausible")
    consumed, text_start, file_end, present = (int(x) for x in lines[-1].split()[1:])
    if consumed != text_start:
        failures.append(
            f"walk ended at {consumed}, text starts at {text_start} -- a length byte is wrong"
        )
    if file_end != present:
        failures.append(f"header says {file_end} bytes, the file has {present}")
    if addresses != sorted(addresses) or len(addresses) != len(set(addresses)):
        failures.append("records are not in strictly increasing address order")
    if not all(0xD000 <= a <= 0xDFFF for a in addresses):
        failures.append("a record is outside $D000-$DFFF")
    return addresses


def check_lookups(binary: Path, addresses: list[int], failures: list[str]) -> None:
    """Field indices must stay inside the field list, and names must be names."""
    replies = run(binary, [f"lookup {a:04x}" for a in addresses])
    for position, address in enumerate(addresses):
        head, indices, _fields = replies[position * 3 : position * 3 + 3]
        if head == "none":
            failures.append(f"${address:04X} is in the table but did not look up")
            continue
        _chip, fields, name = head.split(None, 2)
        fields = int(fields)
        if fields > 8:
            failures.append(f"${address:04X} claims {fields} fields")
        for bit, value in enumerate(int(x) for x in indices.split()):
            # `fields` itself is the "no name here" code, so it is the only
            # value above the last real field that may appear.
            if value > fields:
                failures.append(f"${address:04X} bit {7 - bit} indexes field {value} of {fields}")
        if fields == 8 and any(int(x) == 8 for x in indices.split()):
            failures.append(f"${address:04X} has 8 fields and still marks a bit unnamed")
        if name != "-" and not name.strip():
            failures.append(f"${address:04X} has an empty register name")


def check_descriptions(binary: Path, addresses: list[int], failures: list[str]) -> None:
    """Every description must resolve, and be something the screen can show."""
    asked = [f"text {a:04x} -1" for a in addresses]
    asked += [f"text {a:04x} {bit}" for a in addresses for bit in range(8)]
    replies = run(binary, asked)
    if len(replies) != len(asked):
        failures.append(f"{len(asked)} descriptions asked for, {len(replies)} came back")
        return
    allowed = set(" !\"#$%&'()*+,-./0123456789:;<=>?@[]ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    found = 0
    for question, reply in zip(asked, replies, strict=True):
        if reply == "-":
            continue
        found += 1
        stray = sorted(set(reply) - allowed)
        if stray:
            failures.append(f"{question}: description has {stray} which the screen cannot show")
    if found < len(addresses) // 2:
        failures.append(f"only {found} descriptions resolved, which is too few to be right")


def check_row_layout(binary: Path, failures: list[str]) -> None:
    """Columns must land where the header says, and cells must be name or circle."""
    header = decode(run(binary, ["header"])[0])
    if len(header) != ROW_WIDTH:
        failures.append(f"header is {len(header)} wide, expected {ROW_WIDTH}")
    for bit in range(8):
        column = BIT_COLUMN + bit * BIT_STRIDE
        if header[column] != 0x30 + (7 - bit):
            failures.append(f"header column {column} does not label bit {7 - bit}")

    cases = [
        (0xFFD3011, 0x9B),  # named bits, chip in NAME
        (0xFFD3015, 0x03),  # register mnemonic, no named bits
        (0xFFD3016, 0xC8),  # some bits unnamed
        (0xFFD3404, 0x41),  # register name from the ! head
        (0xFFD3020, 0xFF),  # promoted to a byte value
        (0x00007A0, 0xA5),  # nothing in the database at all
    ]
    replies = run(binary, [f"row {a:x} {v:02x} 255" for a, v in cases])
    for (address, value), reply in zip(cases, replies, strict=True):
        codes = decode(reply.split()[1])
        where = f"${address:07X}"
        if len(codes) != ROW_WIDTH:
            failures.append(f"{where} row is {len(codes)} wide, expected {ROW_WIDTH}")
            continue
        if codes[0] != ord(":"):
            failures.append(f"{where} row does not open with the address marker")
        # The value column, read back as two hex digits.  nybl_to_screen puts
        # 0-9 at $30-$39 but A-F at $01-$06, since those are letters in screen
        # codes rather than the ASCII run above the digits.
        digits = "".join(
            f"{c - 0x30:X}" if c >= 0x30 else f"{c + 9:X}"
            for c in (codes[VALUE_COLUMN], codes[VALUE_COLUMN + 1])
        )
        if digits != f"{value:02X}":
            failures.append(f"{where} VAL shows {digits!r}, expected {value:02X}")
        cells = [
            codes[BIT_COLUMN + bit * BIT_STRIDE : BIT_COLUMN + bit * BIT_STRIDE + BIT_WIDTH]
            for bit in range(8)
        ]
        # A multi-bit field is named over its highest bit and blank across the
        # rest of its span, as the User Guide's tables merge the cell, so the
        # same name must never appear twice in a row.
        for left, right in itertools.pairwise(cells):
            if left == right and left[0] != GLYPH_BIT and left[0] != ord(" "):
                failures.append(f"{where} repeats {text(left).strip()!r} instead of merging")
        for bit in range(8):
            cell = cells[bit]
            if cell[0] == GLYPH_BIT:
                if any(c != ord(" ") for c in cell[1:]):
                    failures.append(f"{where} bit {7 - bit} circle has text after it")
            elif not all(c in NAME_CODES or c == ord(" ") for c in cell):
                failures.append(f"{where} bit {7 - bit} cell is {text(cell)!r}")
        if any(c == 0 for c in codes):
            failures.append(f"{where} row contains a NUL: {text(codes)!r}")


def check_cursor_ring(binary: Path, failures: list[str]) -> None:
    """Nine cells in a ring, both directions, every starting point.

    The emulator cannot press a shifted cursor key, so left is unreachable
    there; this is the only coverage it gets.
    """
    order = [CELL_VALUE, *range(7, -1, -1)]  # value field, then bits 7..0
    replies = run(binary, [f"cursor {cell}" for cell in order])
    for position, (cell, reply) in enumerate(zip(order, replies, strict=True)):
        left, right = (int(x) for x in reply.split())
        want_right = order[(position + 1) % len(order)]
        want_left = order[position - 1]
        if right != want_right:
            failures.append(f"cursor right from {cell} is {right}, expected {want_right}")
        if left != want_left:
            failures.append(f"cursor left from {cell} is {left}, expected {want_left}")


def check_wrap(binary: Path, failures: list[str]) -> None:
    """The line-breaking, over the shapes that actually caused trouble.

    A wrap that split text already fitting the row reached a screenshot before
    anything noticed, so the properties are checked rather than one example.
    """
    bodies = [
        "RASTER COMPARE BIT 8",
        "BITMAP MODE",
        "A",
        "",
        "SUPERCALIFRAGILISTIC" * 5,
        "ENABLE VIC-II HOT REGISTERS. WHEN ENABLED, TOUCHING MANY VIC-II REGISTERS "
        "CAUSES THE VIC-IV TO RECALCULATE DISPLAY PARAMETERS, SUCH AS BORDER POSITIONS",
        " LEADING AND TRAILING ",
        "TWO  CONSECUTIVE  SPACES",
    ]
    rooms = (71, 40, 10, 1)
    asked = [f"wrap {room} {body}" for body in bodies for room in rooms]
    replies = run(binary, asked)

    at = 0
    for body in bodies:
        for room in rooms:
            lines = []
            while at < len(replies) and replies[at] != ".":
                lines.append(replies[at].removeprefix("="))
                at += 1
            at += 1
            where = f"wrap room={room} {body[:24]!r}"
            if any(len(line) > room for line in lines):
                failures.append(f"{where}: a line is wider than the row")
            if "".join(lines) != body:
                failures.append(f"{where}: text was lost or duplicated")
            if len(body) <= room and len(lines) > 1:
                failures.append(f"{where}: split text that fitted in one row")
            if body and not lines:
                failures.append(f"{where}: produced nothing")


def check_arithmetic(binary: Path, failures: list[str]) -> None:
    """toggle and hex entry, over every input they can be given."""
    commands = [f"toggle {v:02x} {b}" for v in range(256) for b in range(8)]
    commands += [f"hex {v:02x} {d:x}" for v in range(256) for d in range(16)]
    replies = run(binary, commands)
    position = 0
    for value in range(256):
        for bit in range(8):
            if int(replies[position], 16) != value ^ (1 << bit):
                failures.append(f"toggle {value:02X} bit {bit} is wrong")
            position += 1
    for value in range(256):
        for digit in range(16):
            if int(replies[position], 16) != ((value << 4) | digit) & 0xFF:
                failures.append(f"hex {value:02X} digit {digit:X} is wrong")
            position += 1


def check_against_iomap(binary: Path, iomap: str, failures: list[str]) -> None:
    """Diff every bit's decoded name against a fresh parse of iomap.txt.

    The one check that can catch the generator and the decoder agreeing on
    something wrong: it re-derives what each bit should be called and compares
    that with what walking the encoded table actually produces.
    """
    sys.path.insert(0, str(ROOT / "tools"))
    import gen_iomap_names as generator

    registers = generator.parse(iomap)
    order = sorted(registers)
    replies = run(binary, [f"lookup {a:04x}" for a in order])
    for position, address in enumerate(order):
        head, indices, field_line = replies[position * 3 : position * 3 + 3]
        if head == "none":
            failures.append(f"${address:04X} is in iomap.txt but not in the table")
            continue

        _display, want_names, want_index = generator.describe(address, registers[address])
        got_names = field_line.split()
        got_index = [int(x) for x in indices.split()]  # bit 7 first

        if not want_names:
            continue
        if got_names != want_names:
            failures.append(f"${address:04X} names {got_names}, iomap.txt gives {want_names}")
            continue
        for position_in_row, bit in enumerate(range(7, -1, -1)):
            index = got_index[position_in_row]
            got = got_names[index] if index < len(got_names) else None
            expected = want_names[want_index[bit]] if bit in want_index else None
            if got != expected:
                failures.append(f"${address:04X} bit {bit} is {got}, iomap.txt gives {expected}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True, help="host C compiler")
    parser.add_argument("--iomap", help="mega65-core iomap.txt, for the name diff")
    args = parser.parse_args()

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        binary = build_harness(Path(tmp), args.cc)
        addresses = check_walk(binary, failures)
        check_lookups(binary, addresses, failures)
        check_row_layout(binary, failures)
        check_cursor_ring(binary, failures)
        check_wrap(binary, failures)
        check_descriptions(binary, addresses, failures)
        check_arithmetic(binary, failures)
        if args.iomap:
            check_against_iomap(binary, args.iomap, failures)

    if failures:
        for failure in failures[:40]:
            print(failure, file=sys.stderr)
        print(f"{len(failures)} problems", file=sys.stderr)
        return 1
    print(f"{len(addresses)} records walked; rows, text, cursor and arithmetic check out")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
