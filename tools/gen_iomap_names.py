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

# The chip index and the three record flags share one byte.
FLAG_CHIP_MASK = 0x1F
FLAG_HAS_FIELDS = 0x20
FLAG_HAS_NAME = 0x40
FLAG_HAS_TEXT = 0x80

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

# Registers whose whole content is that writing them does something.  The 64
# HTRAP entries at $D640-$D67F are hypervisor trap doors: they carry no named
# bits, and in a freeze slot they are bytes nothing reads, so the browser spent
# 704 of its 6636 bytes -- 14% of the addresses -- saying "HTRAP2C" beside eight
# circles.  Matched against the register's own name, not the address, so a
# renumbering upstream does not quietly reinstate them.
DROP_REGISTERS = ("HTRAP",)

# Bits iomap.txt cannot describe, because a CIA port's meaning comes from what
# is wired to the pins rather than from the CIA: it documents $DD00 as "Port A"
# and stops.  These are the C64's, from the Programmer's Reference Guide.
#
# $DC00/$DC01 are deliberately absent.  They are the keyboard matrix and the
# joysticks at the same time -- bit 4 is "fire" or "column 4" depending on what
# the reader is doing -- and a name that is wrong half the time is worse than a
# circle.  $DD01 is the user port, which no fixed name describes either.
CURATED_BITS = {
    0xDD00: {
        7: "DATIN",
        6: "CLKIN",
        5: "DATOUT",
        4: "CLKOUT",
        3: "ATNOUT",
        2: "RSTXD",
        1: "VBANK",
        0: "VBANK",
    },
}

LINE = re.compile(
    r"^(\S+)\s+\$([0-9A-Fa-f]+)(?:\.(\d)(?:-(\d))?)?\s+([^:\s]+):(\S+)\s*(.*)$",
)

# What MONITOR's map display prints in its provenance column, in the order of
# CpuMapMechanism in src/monitor/cpumap.h -- the enum indexes these tables, so
# the two orders are one fact written twice and must not drift.
#
# Entries naming a register take both the mnemonic and the bit position from
# iomap.txt, so a name can never end up printed beside the wrong bit.  Entries
# with no register are MONITOR's own vocabulary, already on its register line.
#                  address  signal      literal
CPUMAP_MECHANISMS = [
    # Unmapped RAM is the absence of a mechanism, so there is nothing to name;
    # the slot exists only to keep the enum indexing the tables directly.
    (None, None, ""),
    (0x0000001, "C64MAP", None),
    (0xD030, "ROM8", None),
    (0xD030, "ROMA", None),
    (0xD030, "ROMC", None),
    (0xD030, "ROME", None),
    (0xD030, "CRAM2K", None),
    (None, None, "MAPLO"),
    (None, None, "MAPHI"),
]


class Register:
    """One I/O address: its chip, its own name, and a name per bit."""

    def __init__(self) -> None:
        self.chip: tuple[int, str] | None = None
        self.name: tuple[int, str] | None = None
        self.bit: dict[int, tuple[int, str]] = {}
        self.text: tuple[int, str] | None = None
        self.bit_text: dict[int, tuple[int, str]] = {}

    def offer_chip(self, rank: int, chip: str) -> None:
        if self.chip is None or self.chip[0] <= rank:
            self.chip = (rank, chip)

    def offer_name(self, rank: int, name: str) -> None:
        if self.name is None or self.name[0] <= rank:
            self.name = (rank, name)

    def offer_bit(self, rank: int, bit: int, name: str) -> None:
        if bit not in self.bit or self.bit[bit][0] <= rank:
            self.bit[bit] = (rank, name)

    def offer_text(self, rank: int, text: str) -> None:
        if text and (self.text is None or self.text[0] <= rank):
            self.text = (rank, text)

    def offer_bit_text(self, rank: int, bit: int, text: str) -> None:
        if text and (bit not in self.bit_text or self.bit_text[bit][0] <= rank):
            self.bit_text[bit] = (rank, text)


def parse(path: str) -> dict[int, Register]:
    registers: dict[int, Register] = defaultdict(Register)
    dropped: set[int] = set()
    with open(path, encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()
    for line in lines:
        match = LINE.match(line.rstrip())
        if not match:
            continue
        chipset, address, high, low, chip, signal, description = match.groups()
        value = int(address, 16)
        if not IO_FIRST <= value <= IO_LAST:
            continue
        # SUMMARY groups a whole chip's registers for the User Guide's overview
        # tables; it names no hardware.
        if chip.upper() == "SUMMARY" or chip not in KEEP_CHIPS:
            continue
        if signal.startswith(DROP_REGISTERS):
            dropped.add(value)
            continue
        rank = CHIPSET_RANK.get(chipset, 0)
        register = registers[value]
        register.offer_chip(rank, chip)
        signal = signal.split("@")[0]
        if high is None:
            register.offer_name(rank, signal)
            register.offer_text(rank, description)
        else:
            first, last = int(high), int(low) if low else int(high)
            for bit in range(min(first, last), max(first, last) + 1):
                register.offer_bit(rank, bit, signal)
                register.offer_bit_text(rank, bit, description)

    # A drop that stops matching is a silent way to grow the table back.
    if not dropped:
        raise SystemExit(f"DROP_REGISTERS {DROP_REGISTERS} matched nothing in {path}")
    return dict(registers)


def bit_masks(path: str) -> dict[tuple[int, str], int]:
    """Every (address, signal) in iomap.txt, with the bits that signal covers.

    Its own pass rather than parse()'s, because the map display needs `$01`,
    which sits far outside the `$D000-$DFFF` window the browser keeps.
    """
    masks: dict[tuple[int, str], int] = defaultdict(int)
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = LINE.match(line.rstrip())
            if not match:
                continue
            _, address, high, low, _, signal, _ = match.groups()
            if high is None:
                continue
            first, last = int(high), int(low) if low else int(high)
            key = (int(address, 16), signal.split("@")[0].split("!")[-1])
            for bit in range(min(first, last), max(first, last) + 1):
                masks[key] |= 1 << bit
    return dict(masks)


def build_labels(path: str) -> str:
    """The provenance table MONITOR's map display indexes by CpuMapMechanism."""
    masks = bit_masks(path)
    names: list[str] = []
    values: list[int] = []
    registers: list[str] = []

    for address, signal, literal in CPUMAP_MECHANISMS:
        if literal is not None:
            names.append(literal)
            values.append(0)
            registers.append("")
            continue
        mask = masks.get((address, signal))
        if mask is None:
            # Never guess a position for a name: a mask beside the wrong
            # mnemonic reads as correct and is not.
            raise SystemExit(f"${address:X} {signal} is not in {path}")
        names.append(signal)
        values.append(mask)
        registers.append(f"${address:02X}" if address < 0x100 else f"${address:04X}")

    # Only mechanisms with a mask ever have their register printed, so the
    # placeholder the literal entries carry needs no row of its own.
    unique = sorted({name for name in registers if name})
    width = max(len(name) for name in names) + 1
    lines = [
        "/* Generated by tools/gen_iomap_names.py from mega65-core's iomap.txt.",
        " * Do not edit: run the cpumap-labels target instead.",
        " *",
        " * Indexed by CpuMapMechanism, which must stay in the same order. */",
        "",
        f"static const char CPUMAP_NAME[][{width}] = {{",
        *(f'    "{name}",' for name in names),
        "};",
        "",
        "static const uint8_t CPUMAP_MASK[] = {",
        "    " + ", ".join(f"0x{value:02X}" for value in values),
        "};",
        "",
        "static const uint8_t CPUMAP_REGISTER[] = {",
        "    " + ", ".join(str(unique.index(name) if name else 0) for name in registers),
        "};",
        "",
        f"static const char CPUMAP_REGISTER_NAME[][{max(len(n) for n in unique) + 1}] = {{",
        *(f'    "{name}",' for name in unique),
        "};",
        "",
    ]
    return "\n".join(lines)


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

    if address in CURATED_BITS and not names:
        # Only where upstream says nothing: if it ever documents these bits, its
        # names win and this entry quietly stops applying.
        curated = CURATED_BITS[address]
        ordered = list(dict.fromkeys(curated[bit] for bit in range(7, -1, -1)))
        return display, ordered, {bit: ordered.index(curated[bit]) for bit in range(8)}

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


# The descriptions are LaTeX sources, not plain text.  Only these five forms are
# markup; everything else that looks like it is not.
LATEX = {
    r"$\times$": "X",
    r"$\div$": "/",
    r"\$": "$",
    r"\#": "#",
    r"\&": "&",
    "``": '"',
    "''": '"',
    "`": "'",
}

# What is left once those are applied.  A bare `$` is a hex prefix far more
# often than a math delimiter -- "Write $45 then $54", "$D000-$DFFF" -- so
# stripping it corrupts the text; `~` means "approximately" (~3.5MHz), not a
# non-breaking space; `_` and `#` are literal, as in f011_rsector_found and
# "thumb wheel #3".  Anything matching this is markup nobody has taught us.
UNKNOWN_MARKUP = re.compile(r"\\[a-zA-Z]+|\\[^a-zA-Z]|[{}]")


def detex(text: str, where: str, warnings: list[str]) -> str:
    """Turn one description from LaTeX into plain text, complaining if it cannot.

    Passing unknown markup through unchanged rather than guessing keeps a future
    iomap.txt visible: the reader sees the backslash and knows to look.
    """
    for source, replacement in LATEX.items():
        text = text.replace(source, replacement)
    leftover = UNKNOWN_MARKUP.findall(text)
    if leftover:
        warnings.append(f"{where}: unhandled markup {sorted(set(leftover))} in {text!r}")
    return " ".join(text.split())


# The screen has no lowercase, `_` or `~` in the uppercase ROM charset.
TEXT_SUBSTITUTIONS = {"_": "-", "~": ""}

# `\` and `^` land on the pound sign and an up arrow in this charset, so they
# would show as something they are not.
UNRENDERABLE = (0x5C, 0x5E)


def to_screen_codes(text: str, where: str = "", warnings: list[str] | None = None) -> list[int]:
    """PETSCII screen codes, not ASCII: the screen renders with the C64 ROM
    charset (see CHARSET_ADDRESS in src/screen.h), so A-Z are $01-$1A and the digits
    sit at $30-$39 as in ASCII.  Every code is below $80, which is what lets the
    caller mark the last character with the terminator in bit 7.

    A name must be representable, so `warnings` left out means raise.  Prose is
    allowed to lose the odd character, so passing a list means warn and drop.
    """
    for source, replacement in TEXT_SUBSTITUTIONS.items():
        text = text.replace(source, replacement)
    codes = []
    for character in text.upper():
        code = ord(character)
        if 0x20 <= code <= 0x3F:
            codes.append(code)  # space through `?`, unchanged
        elif 0x40 <= code <= 0x5D and code not in UNRENDERABLE:
            codes.append(code - 0x40)  # `@`, the letters, and the brackets
        elif warnings is None:
            raise SystemExit(f"name {text!r} has no screen code for {character!r}")
        else:
            warnings.append(f"{where}: dropped {character!r} from a description")
    return codes


def terminated(codes: list[int]) -> list[int]:
    """The last character carries the end of the string in bit 7."""
    codes[-1] |= 0x80
    return codes


# The screen the descriptions have to fit, from bitedit.c: seven shared lines,
# each BITEDIT_ROW_WIDTH less the tag column.  Checked here because the target
# cannot: it would simply draw the eighth line off the bottom of the block.
INFO_ROWS = 7
INFO_ROOM = 78 - 7


def wrapped_lines(text: bytearray, offset: int) -> int:
    """How many lines one pooled string takes, wrapping as bitedit_wrap does."""
    if offset == NO_TEXT:
        return 0
    end = offset
    while not text[end] & 0x80:
        end += 1
    body, at, lines = text[offset : end + 1], 0, 0
    while at < len(body):
        remaining = len(body) - at
        if remaining <= INFO_ROOM:
            return lines + 1
        take = next(
            (i + 1 for i in range(INFO_ROOM - 1, -1, -1) if body[at + i] == 0x20), INFO_ROOM
        )
        at += take
        lines += 1
    return lines


MAGIC = b"M65I"
FORMAT_VERSION = 1
HEADER_BYTES = 12
NO_TEXT = 0xFFFF


def build_binary(registers: dict[int, Register]) -> tuple[bytearray, list[str], list[str]]:
    """The whole file: header, chip names, records, then the description text.

    Descriptions are pooled and shared -- 542 distinct strings across 638 uses --
    so a record carries a two-byte offset into the pool rather than its own copy.
    """
    warnings: list[str] = []
    chips = sorted({r.chip[1] for r in registers.values() if r.chip})
    chip_index = {chip: position for position, chip in enumerate(chips)}
    if len(chips) > FLAG_CHIP_MASK + 1:
        raise SystemExit(f"{len(chips)} chips will not fit the 5-bit field")

    text = bytearray()
    offsets: dict[str, int] = {}

    def pool(raw: str | None, where: str) -> int:
        if not raw:
            return NO_TEXT
        clean = detex(raw, where, warnings)
        if clean not in offsets:
            codes = to_screen_codes(clean, where, warnings)
            if not codes:
                return NO_TEXT
            offsets[clean] = len(text)
            text.extend(terminated(codes))
        return offsets[clean]

    records = bytearray()
    fabricated: list[str] = []
    budgets: list[tuple[int, int, list[int]]] = []
    for address in sorted(registers):
        register = registers[address]
        display, names, bit_index = describe(address, register)
        if len(names) > MAX_FIELDS:
            raise SystemExit(f"${address:04X} has {len(names)} fields, over {MAX_FIELDS}")
        if len(names) != len(set(names)):
            fabricated.append(f"${address:04X}: {names}")

        body = bytearray()
        flags = chip_index[register.chip[1]] if register.chip else 0
        if names:
            flags |= FLAG_HAS_FIELDS
            packed = 0
            for bit in range(7, -1, -1):
                packed = (packed << 3) | bit_index.get(bit, len(names))
            body += bytes(((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF))

        source = register.text[1] if register.text else None
        if source is None and not names and register.bit_text:
            # A register promoted to a byte value keeps its prose.  iomap.txt
            # hung it on the bit range rather than on the register -- $D020 is
            # documented as `$D020.7-0 BORDERCOL display border colour` -- so
            # without this the five colour registers lose their descriptions.
            source = next(iter(register.bit_text.values()))[1]
        register_text = pool(source, f"${address:04X}")
        if register_text != NO_TEXT:
            flags |= FLAG_HAS_TEXT
            body += register_text.to_bytes(2, "little")
        if display:
            flags |= FLAG_HAS_NAME
            body += bytes(terminated(to_screen_codes(display)))

        # One text offset per field, in the order the 3-bit indices select from.
        field_text: list[int] = []
        for position, name in enumerate(names):
            bit = next((b for b in range(8) if bit_index.get(b) == position), None)
            source = register.bit_text.get(bit) if bit is not None else None
            where = pool(source[1] if source else None, f"${address:04X}.{bit}")
            field_text.append(where)
            body += where.to_bytes(2, "little")
            body += bytes(terminated(to_screen_codes(name)))
        budgets.append((address, register_text, field_text))

        length = 4 + len(body)
        if length > 0xFF:
            raise SystemExit(f"${address:04X} record is {length} bytes")
        records += bytes((address & 0xFF, address >> 8, length, flags)) + body

    # A register's own description shares the block with the widest of its bits,
    # so it is the pair that has to fit rather than either one alone.
    for address, register_text, field_text in budgets:
        rows = wrapped_lines(text, register_text) + max(
            (wrapped_lines(text, f) for f in field_text), default=0
        )
        if rows > INFO_ROWS:
            warnings.append(
                f"${address:04X}: description needs {rows} lines, block holds {INFO_ROWS}"
            )

    if fabricated:
        raise SystemExit(
            "abbreviation collided, which would show an invented name:\n  "
            + "\n  ".join(fabricated)
        )

    chip_bytes = bytearray()
    for chip in chips:
        chip_bytes += bytes(terminated(to_screen_codes(chip[:REGISTER_NAME_WIDTH])))

    record_start = HEADER_BYTES + len(chip_bytes)
    text_start = record_start + len(records)
    total = text_start + len(text)
    if total > 0xFFFF:
        raise SystemExit(f"{total} bytes will not fit the 16-bit offsets")

    out = bytearray(MAGIC)
    out += bytes((FORMAT_VERSION, len(chips)))
    out += record_start.to_bytes(2, "little")
    out += text_start.to_bytes(2, "little")
    out += total.to_bytes(2, "little")
    out += chip_bytes + records + text
    return out, chips, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iomap", required=True, help="path to mega65-core's iomap.txt")
    parser.add_argument("-o", "--output", required=True, help="destination IOMAP.M65")
    parser.add_argument("--labels", help="destination for the map display's label table")
    args = parser.parse_args()

    if args.labels:
        with open(args.labels, "w", encoding="utf-8") as handle:
            handle.write(build_labels(args.iomap))
        print(f"map labels -> {args.labels}", file=sys.stderr)

    registers = parse(args.iomap)
    if not registers:
        raise SystemExit(f"no ${IO_FIRST:04X}-${IO_LAST:04X} entries in {args.iomap}")
    data, chips, warnings = build_binary(registers)

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)

    with open(args.output, "wb") as handle:
        handle.write(data)

    named_bits = sum(len(r.bit) for r in registers.values())
    print(
        f"{len(registers)} addresses, {named_bits} named bits, {len(chips)} chips, "
        f"{len(data)} bytes -> {args.output}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
