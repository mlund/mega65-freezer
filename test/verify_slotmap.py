#!/usr/bin/env python3
"""Check that a frozen address lands in the sector the region table says it does.

The freeze slot is a run of SD sectors, not memory, and freeze_peek()/
freeze_poke() transfer a whole 512-byte sector for every single byte.  Which
sector an address falls in therefore decides both correctness and how much SD
traffic a routine costs, and batching several bytes into one transfer is only
sound when they genuinely share a sector.  This pins that.

The region table below is hyppo's own, transcribed from freeze_mem_list in
mega65-core/src/hyppo/freeze.asm -- the entries are 4 bytes of base, 3 of
length, 1 of preparatory action.  What that buys is a check against a second
reading of the same table: _walk() below is a separate implementation, so a
transcription slip in either shows up, but a shared misreading of hyppo's
format would not.  The sector groupings are the part worth having -- they say
which bytes may be batched into one transfer.

    python3 test/verify_slotmap.py --cc cc
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# hyppo's freeze_mem_list, in order.  The two symbolic entries there
# (freeze_scratch_area, currenttask_block) are given the addresses and sizes
# hyppo resolves them to.  Length carries flags above bit 23, which
# REGION_LENGTH_MASK strips -- $FFD4000's $801000 is really $1000, and that
# masking is exactly what the thumbnail lookup depends on.
REGIONS = [
    (0xFFF8000, 0x000100),  # freeze_scratch_area
    (0xFFD6000, 0x000090),  # stashed SD buffer + registers
    (0xFFD6C00, 0x000200),  # F011 sector buffer
    (0xFFFBD00, 0x000100),  # process descriptor
    (0xFFD3640, 0x00003E),  # reset block, $D640-$D67D
    (0xFFD3100, 0x000300),  # SID 1
    (0xFFD3100, 0x000300),  # SID 2
    (0xFFD3100, 0x000300),  # SID 3
    (0xFFD3100, 0x000300),  # SID 4
    (0xFF80000, 0x008000),  # colour RAM
    (0xFFD3C00, 0x000200),  # both CIAs
    (0xFFD3000, 0x000080),  # VIC-IV $D000-$D07F
    (0xFFD0030, 0x000001),  # C128 $D030.0, one byte
    (0x0000000, 0x060000),  # main RAM, 384KB
    (0x0000400, 0x000100),  # currenttask_block
    (0xFFD3710, 0x0000F0),  # DMAgic
    (0xFFDB000, 0x005000),  # ethernet buffers
    (0xFFD4000, 0x801000),  # thumbnail, masked to $1000
    (0xFF7E000, 0x001000),  # character ROM
    (0xFFD3084, 0x00007C),  # VIC-IV $D084-$D0FF
]

# Addresses the freezer actually reads or writes, grouped by the region that
# holds them.  Every address in a group must land in one sector -- that is the
# property batching relies on, and the reason each is listed by name.
SHARE_A_SECTOR = [
    (
        "VIC-IV $D000-$D07F",
        [
            0xFFD3031,
            0xFFD3048,
            0xFFD3049,
            0xFFD304A,
            0xFFD304B,
            0xFFD304E,
            0xFFD304F,
            0xFFD3054,
            0xFFD306F,
            0xFFD3072,
        ],
    ),
    (
        "reset block $D640-$D67D",
        [0xFFD3640, 0xFFD3647, 0xFFD364A, 0xFFD364F, 0xFFD3650, 0xFFD3651, 0xFFD367D],
    ),
    ("both CIAs", [0xFFD3C0E, 0xFFD3D0E]),
    ("process descriptor", [0xFFFBD00, 0xFFFBD11, 0xFFFBD14, 0xFFFBD35, 0xFFFBDFF]),
    ("main RAM reset vector", [0x2FFFC, 0x2FFFD]),
    ("main RAM $10113/4", [0x10113, 0x10114]),
]

# Addresses outside every region: the walk has to report them missing rather
# than return an offset into whatever region happens to follow.
ABSENT = [0xFFD5000, 0x0700000, 0xFFD8000]


def _walk(address: int) -> int | None:
    """Where the region format puts an address, worked out here rather than by
    the code under test: regions lie back to back from the slot's second sector,
    each rounded up to a whole sector, and length carries flags above bit 23."""
    offset = 1  # the slot's first sector holds the saved SD buffer
    for base, length in REGIONS:
        length &= 0x7FFFFF
        relative = address - base
        if base == 0x1000 and (address & 0xFFFF000) == 0xFF54000:
            return (offset << 9) + (address & 0xFFF)
        if address < base or relative >= length:
            offset += (length >> 9) + (1 if length & 0x1FF else 0)
            continue
        offset += relative >> 9
        return (offset << 9) + (relative & 0x1FF)
    return None


def _thumb() -> int | None:
    """The thumbnail's sector, by the same independent walk.  It is a sector
    index rather than a byte offset, and the region is found by base address --
    a bitstream either maps it at $FFD4000 or relocates it to $1000."""
    offset = 1
    for base, length in REGIONS:
        if base in (0x1000, 0xFFD4000):
            return offset
        length &= 0x7FFFFF
        offset += (length >> 9) + (1 if length & 0x1FF else 0)
    return None


def _script() -> tuple[str, list]:
    """The harness input, and what each output line has to be."""
    lines = [f"count {len(REGIONS)}"]
    for index, (base, length) in enumerate(REGIONS):
        lines.append(f"region {index} {base:x} {length:x}")

    checks = []

    def expect(command: str, label: str, want: str, group: str | None = None):
        lines.append(command)
        checks.append((label, want, group))

    # Exact offsets, so an arithmetic slip is caught and not just a regrouping:
    # every region's base, and a byte inside each that is not the first.
    for base, length in REGIONS:
        for address in (base, base + min(0x1FF, (length & 0x7FFFFF) - 1)):
            want = _walk(address)
            expect(
                f"offset {address:x}",
                f"offset of ${address:07X}",
                "NONE" if want is None else f"{want:x}",
            )

    # Sharing a sector is what batching several bytes into one transfer needs.
    for name, addresses in SHARE_A_SECTOR:
        for address in addresses:
            want = _walk(address)
            expect(
                f"sector {address:x}",
                f"{name}: ${address:07X}",
                "NONE" if want is None else f"{want >> 9:x}",
                group=name,
            )

    for address in ABSENT:
        expect(f"offset {address:x}", f"${address:07X} is outside every region", "NONE")

    want = _thumb()
    expect("thumb", "thumbnail region", "NONE" if want is None else f"{want:x}")

    return "\n".join(lines) + "\n", checks


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()

    script, checks = _script()
    with tempfile.TemporaryDirectory() as tmp:
        binary = os.path.join(tmp, "slotmap")
        subprocess.run(
            [
                args.cc,
                "-std=c23",
                "-Wall",
                "-Werror",
                "-o",
                binary,
                os.path.join(HERE, "slotmap_host_harness.c"),
            ],
            check=True,
        )
        got = subprocess.run(
            [binary], input=script, capture_output=True, text=True, check=True
        ).stdout.splitlines()

    if len(got) != len(checks):
        print(f"harness produced {len(got)} lines, expected {len(checks)}")
        return 1

    failures = 0
    groups: dict[str, set] = {}
    for (label, want, group), actual in zip(checks, got, strict=False):
        if actual != want:
            print(f"FAIL  {label}: expected {want!r}, got {actual!r}")
            failures += 1
        if group is not None:
            groups.setdefault(group, set()).add(actual)

    for name, sectors in groups.items():
        if len(sectors) != 1:
            print(f"FAIL  {name}: spans {len(sectors)} sectors {sorted(sectors)}, expected 1")
            failures += 1

    total = len(checks) + len(groups)
    if failures:
        print(f"\n{failures} of {total} checks failed")
        return 1
    print(f"{total} checks passed: every named group shares one sector")
    return 0


if __name__ == "__main__":
    sys.exit(main())
