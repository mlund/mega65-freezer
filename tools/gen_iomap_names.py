#!/usr/bin/env python3
"""Generate the I/O register name table for MONITOR's `B` command.

The source of truth is `iomap.txt` from mega65-core, itself a flat dump of the
`@IO:` comments in `src/vhdl/*.vhdl` -- the same lines the User Guide's register
appendices are built from.  Run via CMake rather than by hand::

    cmake --build build --target iomap-names

Each line reads `<CHIPSET> $ADDR[.bits] <CHIP>:<SIGNAL>[@ALIAS] <description>`.
Only `$D000-$DFFF` is kept, and only for the chips in KEEP_CHIPS.  Where the
same address is documented for more than one chipset the MEGA65 view wins
(GS > C65 > C64), so `$D020` is the 256-colour VIC-IV border register rather
than the 16-colour VIC-II one.

`!` inside a signal name separates the register context from the bit: `$D404`
is documented as `SID:VOICE1!CTRLGATE` with the alias `@VOICEX!CTRLGATE`, which
generalises `VOICE1` to `VOICEX` and so marks the head as context.  Taking the
last segment is what turns that register's bits from `CTRLGATE CTRLRMF ...`
into `GATE RMF RMO TST TRI SAW PUL RNW`.

Abbreviation must never invent text -- a bit browser that shows a made-up name
is worse than one that shows none -- so the run fails if the disambiguating
digit is ever needed.  With the rules below it is not.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections import defaultdict

# MEGA65 first: the freezer edits a MEGA65, so where a register is documented
# for several chipsets the widest view is the right one.
CHIPSET_RANK = {"C64": 1, "C65": 2, "GS": 3}

IO_FIRST = 0xD000
IO_LAST = 0xDFFF

# The chips worth naming a bit at a time.  Everything here is something you
# reach for while looking at a frozen program -- the display, the sound, the
# ports and timers, the disk and the transfers.  The rest of what iomap.txt
# documents is read with M rather than toggled: MATH alone is 133 addresses of
# a unit whose registers are operands, and HCPU, UARTMISC, FPGA, TOUCH, DEBUG
# and QSPI are much the same.  Dropping them pays for CIA1 and CIA2, which the
# old $D7FF ceiling had excluded entirely.
KEEP_CHIPS = {
    "VIC-II",
    "VIC-III",
    "VIC-IV",
    "SID",
    "AUDIO",
    "AUDIOMIX",
    "CIA1",
    "CIA2",
    "DMA",
    "CPU",
    "F011",
    "FDC",
    "SD",
    "MISCIO",
    "ETH",
}

# What fits the table's columns.  The NAME column is 8 wide and the bit columns
# 7, one of which is the separating space.
BIT_NAME_WIDTH = 6
REGISTER_NAME_WIDTH = 8

# A bit cell's name is chosen from at most eight per register, so the index is
# three bits and the count that means "this bit has no name" is the number of
# names present.
MAX_FIELDS = 8

VOWELS = "AEIOU"

# Registers iomap.txt describes as a whole byte -- "sprite enable bits" -- where
# every bit is the same flag for one sprite.  The VHDL says it once instead of
# eight times, so there is nothing per bit to read, and the User Guide's tables
# are generated from those same lines and have the identical gap.  Bit N is
# sprite N (C64 Programmer's Reference Guide, VIC-II register map), so these are
# the documented names rather than invented ones.
PER_SPRITE = {
    0xD010: "SXMSB",
    0xD015: "SE",
    0xD017: "SEXY",
    0xD01B: "BSP",
    0xD01C: "SCM",
    0xD01D: "SEXX",
    0xD01E: "SSC",
    0xD01F: "SBC",
}

LINE = re.compile(
    r"^(\S+)\s+\$([0-9A-Fa-f]+)(?:\.(\d)(?:-(\d))?)?\s+([^:\s]+):(\S+)\s*(.*)$",
)


class Register:
    """One I/O address: its chip, its own name, and a name per bit."""

    def __init__(self) -> None:
        self.chip: tuple[int, str] | None = None
        self.name: tuple[int, str] | None = None
        self.bit: dict[int, tuple[int, str]] = {}

    def offer_chip(self, rank: int, chip: str) -> None:
        if self.chip is None or self.chip[0] <= rank:
            self.chip = (rank, chip)

    def offer_name(self, rank: int, name: str) -> None:
        if self.name is None or self.name[0] <= rank:
            self.name = (rank, name)

    def offer_bit(self, rank: int, bit: int, name: str) -> None:
        if bit not in self.bit or self.bit[bit][0] <= rank:
            self.bit[bit] = (rank, name)


def parse(path: str) -> dict[int, Register]:
    registers: dict[int, Register] = defaultdict(Register)
    with open(path, encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()
    for line in lines:
        match = LINE.match(line.rstrip())
        if not match:
            continue
        chipset, address, high, low, chip, signal, _description = match.groups()
        value = int(address, 16)
        if not IO_FIRST <= value <= IO_LAST:
            continue
        # SUMMARY groups a whole chip's registers for the User Guide's overview
        # tables; it names no hardware.
        if chip.upper() == "SUMMARY" or chip not in KEEP_CHIPS:
            continue
        rank = CHIPSET_RANK.get(chipset, 0)
        register = registers[value]
        register.offer_chip(rank, chip)
        signal = signal.split("@")[0]
        if high is None:
            register.offer_name(rank, signal)
        else:
            first, last = int(high), int(low) if low else int(high)
            for bit in range(min(first, last), max(first, last) + 1):
                register.offer_bit(rank, bit, signal)
    return dict(registers)


def squeeze(name: str, width: int) -> str:
    """Shorten to `width`, dropping the least load-bearing characters first.

    `!` and `-` are separators rather than letters, and a vowel late in a
    mnemonic carries less than the consonants around it, so both go before any
    character is simply cut off the end.
    """
    letters = list(name.replace("!", "").replace("-", ""))
    index = len(letters) - 1
    while len(letters) > width and index > 0:
        if letters[index] in VOWELS:
            del letters[index]
        index -= 1
    return "".join(letters)[:width]


def abbreviate_bits(register: Register) -> tuple[list[str], dict[int, int]]:
    """Name every bit of one register, returning the names and a bit->index map.

    The common prefix strip is what removes `CTRL` from `$D404`'s eight control
    bits: it is shared by all of them, so it distinguishes nothing and only eats
    column width.
    """
    ordered: list[str] = []
    for bit in range(7, -1, -1):
        entry = register.bit.get(bit)
        if entry and entry[1] not in ordered:
            ordered.append(entry[1])

    # The tail alone only names a bit when it is unique here.  `$D6AE` has both
    # AUTO!2XSEL and FDC!2XSEL, and `$D712` both DBGPAN!RGTSAT and DBG!RGTSAT --
    # the head is the whole distinction, so those keep it.
    tails = [name.split("!")[-1] for name in ordered]
    ambiguous = {tail for tail in tails if tails.count(tail) > 1}
    tails = [
        name.replace("!", "") if tail in ambiguous else tail
        for name, tail in zip(ordered, tails, strict=True)
    ]

    prefix = os.path.commonprefix(tails)
    if len(prefix) < 3 or any(len(tail) - len(prefix) < 2 for tail in tails):
        prefix = ""

    names = [squeeze(tail[len(prefix) :], BIT_NAME_WIDTH) for tail in tails]

    index_of = {name: position for position, name in enumerate(ordered)}
    bit_index = {bit: index_of[entry[1]] for bit, entry in register.bit.items()}
    return names, bit_index


def describe(address: int, register: Register) -> tuple[str | None, list[str], dict[int, int]]:
    """What one address shows: its NAME column, its bit names, and the mapping.

    Every editorial rule lands here rather than in build(), so the checker can
    ask the same question instead of re-deriving the answer -- a copy would go
    on asserting the old rule the day one of these changes.
    """
    raw_bit_names = [entry[1] for _, entry in sorted(register.bit.items())]
    names, bit_index = abbreviate_bits(register)
    display = register_display_name(register, raw_bit_names)

    if address in PER_SPRITE and not names:
        return (
            PER_SPRITE[address],
            [f"S{sprite}" for sprite in range(7, -1, -1)],
            {bit: 7 - bit for bit in range(8)},
        )

    # One field covering all eight bits is a byte-wide value, not a set of
    # flags: `$D020` is a border colour.  Naming it once in the NAME column
    # beats repeating BRDRCL across eight cells, and drops the index bytes.
    if len(names) == 1 and len(register.bit) == 8 and display is None:
        return squeeze(raw_bit_names[0].split("!")[-1], REGISTER_NAME_WIDTH), [], {}

    return display, names, bit_index


def register_display_name(register: Register, bit_names: list[str]) -> str | None:
    """The NAME column: the register's own mnemonic, or the bits' shared head.

    `iomap.txt` names most addresses directly.  Where it does not but *every*
    bit carries the same `!` head, that head is the register -- VOICE1!CTRLGATE
    and its seven siblings make `$D404` VOICE1.  A head shared by only some of
    the bits names nothing: `$D054` has SPR!H640 beside CHR16 and ALPHEN.
    """
    if register.name is not None:
        return squeeze(register.name[1], REGISTER_NAME_WIDTH)
    if not bit_names or not all("!" in name for name in bit_names):
        return None
    heads = {name.split("!")[0] for name in bit_names}
    return squeeze(heads.pop(), REGISTER_NAME_WIDTH) if len(heads) == 1 else None


# PETSCII screen codes, not ASCII: the screen renders with the C64 ROM charset
# (see the monitor's charset note), so A-Z are $01-$1A and the digits sit at
# $30-$39 as in ASCII.  Every code is below $80, which is what lets the last
# character of a name carry the terminator in bit 7.
def to_screen_codes(text: str) -> list[int]:
    codes = []
    for character in text:
        if "A" <= character <= "Z":
            codes.append(ord(character) - 0x40)
        elif "0" <= character <= "9" or character == "-":
            codes.append(ord(character))
        else:
            raise SystemExit(f"name {text!r} has no screen code for {character!r}")
    codes[-1] |= 0x80
    return codes


def build(registers: dict[int, Register]) -> tuple[list[int], list[str], dict[str, int]]:
    """Encode every address into the byte stream the C decoder walks.

    Record: addr16, length, chip|flags, then 3 index bytes when the register has
    named bits, then the names -- the register's own first when present.  The
    length byte is what makes the walk one addition rather than a decode.
    """
    chips = sorted({r.chip[1] for r in registers.values() if r.chip})
    chip_index = {chip: position for position, chip in enumerate(chips)}
    if len(chips) > 32:
        raise SystemExit(f"{len(chips)} chips will not fit the 5-bit field")

    stream: list[int] = []
    fabricated: list[str] = []

    for address in sorted(registers):
        register = registers[address]
        display, names, bit_index = describe(address, register)

        if len(names) > MAX_FIELDS:
            raise SystemExit(f"${address:04X} has {len(names)} fields, over {MAX_FIELDS}")
        if len(names) != len(set(names)):
            fabricated.append(f"${address:04X}: {names}")

        flags = chip_index[register.chip[1]] if register.chip else 0
        if names:
            flags |= 0x20
        if display:
            flags |= 0x40

        body: list[int] = []
        if names:
            # Eight 3-bit indices, bit 7 of the register down to bit 0.  A bit
            # with no name takes the index one past the last, which is why an
            # eight-name register never needs one.
            packed = 0
            for bit in range(7, -1, -1):
                packed = (packed << 3) | bit_index.get(bit, len(names))
            body += [(packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF]
        if display:
            body += to_screen_codes(display)
        for name in names:
            body += to_screen_codes(name)

        length = 4 + len(body)
        if length > 0xFF:
            raise SystemExit(f"${address:04X} record is {length} bytes")
        stream += [address & 0xFF, address >> 8, length, flags, *body]

    if fabricated:
        raise SystemExit(
            "abbreviation collided, which would show an invented name:\n  "
            + "\n  ".join(fabricated)
        )
    return stream, chips, chip_index


def render(stream: list[int], chips: list[str], registers: dict[int, Register]) -> str:
    # Not squeeze()d: the hyphen is part of the name, so VIC-IV must not become
    # VICIV.  All 29 already fit the column, UARTMISC exactly.
    chip_codes: list[int] = []
    for chip in chips:
        chip_codes += to_screen_codes(chip[:REGISTER_NAME_WIDTH])

    out = [
        "// Generated by tools/gen_iomap_names.py -- do not edit.",
        "//",
        "// Regenerate with:  cmake --build build --target iomap-names",
        "//",
        "// Names for the I/O registers, from mega65-core's iomap.txt.  Text is in",
        "// PETSCII screen codes with bit 7 set on a name's last character; see the",
        "// generator for the record layout and the abbreviation rules.",
        "",
        f"#define IOMAP_CHIP_COUNT {len(chips)}",
        f"#define IOMAP_TABLE_BYTES {len(stream)}",
        "",
        "// Chip names in index order, for the NAME column's last resort.",
        f"static const uint8_t IOMAP_CHIPS[{len(chip_codes)}] IOMAP_RODATA = {{",
    ]
    out += emit_bytes(chip_codes)
    out += [
        "};",
        "",
        "// addr16, length, chip|flags, [3 index bytes], [register name], [bit names].",
        "// Sorted by address, so a forward walk serves a whole screen of rows.",
        f"static const uint8_t IOMAP_TABLE[{len(stream)}] IOMAP_RODATA = {{",
    ]
    out += emit_bytes(stream)
    out += ["};", ""]

    named_bits = sum(len(r.bit) for r in registers.values())
    out.insert(
        7,
        f"// {len(registers)} addresses, {named_bits} named bits, {len(chips)} chips.",
    )
    return "\n".join(out)


def emit_bytes(data: list[int]) -> list[str]:
    return [
        "    " + " ".join(f"0x{byte:02x}," for byte in data[start : start + 12])
        for start in range(0, len(data), 12)
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iomap", required=True, help="path to mega65-core's iomap.txt")
    parser.add_argument("-o", "--output", required=True, help="destination .inc file")
    args = parser.parse_args()

    registers = parse(args.iomap)
    if not registers:
        raise SystemExit(f"no ${IO_FIRST:04X}-${IO_LAST:04X} entries in {args.iomap}")
    stream, chips, _ = build(registers)

    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write(render(stream, chips, registers))

    print(
        f"{len(registers)} addresses, {len(chips)} chips, {len(stream)} bytes -> {args.output}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
