#!/usr/bin/env python3
"""Edit a bit in the monitor and read the byte back off the card.

The host test checks the name table; this checks the parts only a machine has.
`B` draws a register's bits from the database, a keypress toggles one, leaving
the editor writes the sector, and a later `M` fetches that sector again.  The
write and the read are separate code, so agreement between them means the byte
really reached the freeze slot rather than merely surviving in the monitor's
sector cache.

It needs an SD image because the freezer loads its tools from the card, and
Xemu's own image carries stock MEGA65 binaries -- without this the test would
exercise upstream's monitor rather than ours.  The image is cloned before our
builds are copied in, so the one passed in is never modified.


With --expect-serial the emulator also runs -hyperserialascii and the trace
edit_bits() emits on each write is checked.  That is the stronger assertion:
it is plain text, so it does not depend on the screen's colours, and it shows
every write rather than only the byte left behind.  It needs a build configured
with -DFREEZER_TRACE=ON.

With --without-iomap the database is left off the card, which must leave the
editor working but unnamed: a missing file is deliberately not an error, so
this is the only run that proves it degrades rather than hangs or refuses.

    python3 test/verify_bitedit_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

from __future__ import annotations

import argparse
import contextlib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario
import m65harness
from m65harness import Failure

# $D011 in the frozen machine.  Its bit 5 is BMM, which the database names, so
# a screen carrying "BMM" proves the lookup and the abbreviation as well as the
# draw.  Editing it cannot disturb the freezer's own display: the frozen
# program's registers live in the freeze slot, not in the VIC.
TARGET = "ffd3011"

OPENING = f"b {TARGET}\r"
CLOSING = f"m {TARGET}\r"

# Low RAM, far enough from $FFD3xxx to be a different freeze-slot sector.  The
# monitor caches one sector at a time, so reading here evicts the edited one and
# the reopen below has to fetch it from the card again.  Without this step the
# value could be coming straight back out of mon_sector and the card never
# written at all.
ELSEWHERE = "m 0000400\r"

# What the table must show before anything is touched.  The last of these comes
# from IOMAP.M65's description pool rather than its names, so it fails if the
# database did not reach the card -- which the names alone would not catch,
# since a missing file looks exactly like a register nobody documented.
# The editor opens with the cursor on bit 7, so RC8 is the described bit.
NAMED_SCREEN = ["VIC-II", "BMM", "RSEL", "YSCL", "RASTER COMPARE BIT 8"]

# The address row, which draws in both configurations.
ADDRESS_ROW = f":{TARGET.upper()}"


def wanted_on_screen(with_database: bool) -> tuple[list[str], list[str]]:
    """What the table must and must not show, given whether the database is on
    the card.  Chosen once: the wait for the table to draw and the assertion
    afterwards are the same fact, and stating it twice let them disagree."""
    if with_database:
        return [ADDRESS_ROW, *NAMED_SCREEN], []
    # No database must leave the editor working but unnamed -- a missing file
    # is deliberately not an error.
    return [ADDRESS_ROW], NAMED_SCREEN


# Typed into the VAL field last.  Not a value a toggle could have produced, so
# reading it back proves hex entry rather than only the bit flip.
EXPECTED_VALUE = "3B"


def find_edit(original: str, clone: str, after: int) -> tuple[int, list[int]]:
    """Find the edited byte in the freeze slot, by diffing the clone with its source.

    The reopen proves the byte comes back through the monitor; this proves it
    reached the card, which no amount of reading through the same code can.

    Which sector holds it depends on the region list, and that comes from the
    hypervisor rather than the card, so it cannot be computed here.  The offset
    within the sector can: address_to_freeze_slot_offset() ends with
    `+= relative_address & 0x1FF`, and the region containing the I/O personality
    is sector aligned, so $FFD3011 sits at byte $11.  Confirmed by running the
    same edit twice with different values -- exactly one byte on the whole card
    took both, at sector 4196943 offset 17.
    """
    offset = int(TARGET, 16) & 0x1FF
    written = 0
    carrying = []
    with open(original, "rb") as left, open(clone, "rb") as right:
        sector = 0
        while True:
            a, b = left.read(512), right.read(512)
            if not a or not b:
                break
            if a != b:
                written += 1
                if b[offset] == after and a[offset] != after:
                    carrying.append(sector)
            sector += 1
    return written, carrying


def drive(machine, required: list[str]):
    """Exercise both ways of changing a byte, then prove the result persisted.

    Both editing paths are covered because they are separate code: SPACE goes
    through toggle_bit on a single bit, hex entry through enter_hex_digit on the
    whole byte.  Typing $3B last means the final value cannot be explained by
    the toggle alone.

    The eviction is what makes the reopen meaningful: leaving flushes the sector
    to the card, reading elsewhere displaces it from the one-sector cache, so
    the reopen has to fetch the byte back off the card.  Reopening also puts the
    names back on screen, which M's sixteen lines would otherwise push off.
    """

    def until(phrase, what):
        return machine.wait_until(lambda s: phrase in s, what=what)

    machine.type_text("m")
    until("PC   IRQ  NMI", "the monitor never started")

    machine.type_text(OPENING)
    for phrase in required:
        until(phrase, f"the bit table never showed {phrase!r}")

    # The cursor opens on bit 7 and RIGHT walks down, so two presses reach bit 5.
    # A toggled bit shows only as the table changing, so that is what says the
    # keypress landed.
    was = machine.snapshot().whole()
    for _ in range(2):
        machine.press("right")
    machine.type_text(" ")
    machine.wait_until(lambda s: s.whole() != was, what="SPACE toggled nothing")

    # Onward to the value field rather than back to it: a shifted cursor key
    # does not survive the virtual key slots -- LEFT arrives as RIGHT -- so the
    # test relies on the cursor ring wrapping past bit 0.
    for _ in range(6):
        machine.press("right")
    machine.type_text(EXPECTED_VALUE.lower())
    until(EXPECTED_VALUE, f"{EXPECTED_VALUE} never reached the value field")

    machine.press("stop")
    machine.type_text(CLOSING)
    until(f":{TARGET.upper()}", "leaving the editor did not reach the monitor")

    machine.type_text(ELSEWHERE)
    until(":0000400", "the displacing read never happened")

    machine.type_text(OPENING)
    shot = machine.snapshot()
    for phrase in required:
        shot = until(phrase, f"the bit table did not bring {phrase!r} back")
    # Handed back rather than re-read by the caller: this is the screen the
    # assertions are about, and reading it twice is a whole screen of traffic.
    return shot


def main() -> int:
    parser = scenario.add_arguments(argparse.ArgumentParser(description=__doc__))
    parser.add_argument(
        "--expect-serial",
        action="store_true",
        help="also require the FREEZER_TRACE write log; needs -DFREEZER_TRACE=ON",
    )
    parser.add_argument(
        "--without-iomap",
        action="store_true",
        help="leave IOMAP.M65 off the card: the editor must still work, unnamed",
    )
    parser.add_argument(
        "--screenshot",
        help="write a PNG on exit; the only way to judge the bright/dim difference",
    )
    args = parser.parse_args()

    required, forbidden = wanted_on_screen(not args.without_iomap)

    with scenario.staged_card(args, with_iomap=not args.without_iomap) as (clone, tmp):
        serial_path = os.path.join(tmp, "serial.txt")
        extra = []
        if args.expect_serial:
            # Not -hyperdebug: that is a separate, very spammy hypervisor trace
            # and it drowns the channel this reads.
            extra.append("-hyperserialascii")
        if args.screenshot:
            extra += ["-screenshot", os.path.abspath(args.screenshot)]

        with contextlib.ExitStack() as stack:
            log = None
            if args.expect_serial:
                log = stack.enter_context(open(serial_path, "w", encoding="utf-8"))
            with scenario.machine_on(args, clone, extra=extra, stdout=log) as machine:
                try:
                    screen = drive(machine, required).whole()
                except Failure as failed:
                    return m65harness.report_failure(failed)

        serial = ""
        if args.expect_serial:
            with open(serial_path, encoding="utf-8", errors="replace") as handle:
                serial = handle.read()
        written, carrying = find_edit(args.sdimg, clone, int(EXPECTED_VALUE, 16))

    problems = [f"screen never showed {want!r}" for want in required if want not in screen]
    problems += [f"screen showed {seen!r} with no database" for seen in forbidden if seen in screen]

    if not written:
        problems.append("nothing was written to the card at all")
    elif not carrying:
        problems.append(
            f"no sector on the card holds {EXPECTED_VALUE} at byte "
            f"${int(TARGET, 16) & 0x1FF:02X}, so the freeze slot never took the edit "
            f"({written} sectors written)"
        )

    # The reopened table's own VAL column: the byte as it stands after the
    # editor was left, the sector flushed, and that sector displaced from the
    # cache, so this value came back off the card.
    reread = re.search(r":FFD3011\s+\S*\s+([0-9A-F]{2})\s", screen)
    if not reread:
        problems.append("the reopened table did not show $FFD3011")
    elif reread.group(1) != EXPECTED_VALUE:
        problems.append(f"VAL reads {reread.group(1)}, expected {EXPECTED_VALUE}")

    if args.expect_serial:
        # Xemu wraps each message: Hypervisor serial output: "BITEDIT ...".
        if args.without_iomap:
            if "IOMAP MISSING" not in serial:
                problems.append("no database on the card, but nothing reported it missing")
        elif "IOMAP LOADED" not in serial:
            problems.append(
                "the database did not load: "
                + ("reported missing" if "IOMAP MISSING" in serial else "no IOMAP trace at all")
            )
        writes = re.findall(r"BITEDIT ([0-9A-F]{7}) ([0-9A-F]{2})", serial)
        if not writes:
            problems.append("no BITEDIT trace on the serial channel")
        elif not any(address.endswith("FD3011") for address, _ in writes):
            problems.append(f"trace never wrote {TARGET}: {writes}")

    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        sys.stdout.write(f"Screen was:\n{screen}\n")
        return 1
    print(
        f"${TARGET.upper()} = {reread.group(1)} off the card; "
        f"{written} sectors written, slot byte found in {carrying}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
