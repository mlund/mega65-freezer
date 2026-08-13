#!/usr/bin/env python3
"""Check the frozen CPU's address translation against the hardware's own logic.

A 16-bit address in the monitor is meaningless until four mechanisms have had
their say -- MAP, VIC-III `$D030` banking, the C64 `$01` port, and the I/O
window -- and they do not compose in the order most descriptions give.  Getting
the order wrong is invisible: every address still resolves to *something*, and
the wrong answer is a plausible one in the wrong bank.

The oracle below is a transcription of `resolve_address_to_long` in
mega65-core/src/vhdl/gs4510.vhdl (~line 9047), which is the hardware.  What
that buys is a check against a second reading of the same authority rather than
against cpumap.c's own arithmetic; the parts most worth having are the
precedence cases, where two mechanisms both want an address.

Two of the VHDL's inputs are deliberately absent.  The cartridge port (EXROM,
GAME) is lines rather than registers, so a freeze slot cannot record it, and
hypervisor mode never applies to a frozen program.

A transcription goes stale silently: upstream can rewrite the function while
every check here keeps passing.  So the version it was taken from is recorded
below, and `--core` re-reads the function and compares a digest, which turns a
silent drift into a failure naming what to re-read.

    python3 test/verify_cpumap.py --cc cc [--core ~/mega65-core]
"""

import argparse
import hashlib
import itertools
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostbuild

HERE = os.path.dirname(os.path.abspath(__file__))

# The mega65-core the oracle below was transcribed from, and the digest of
# `resolve_address_to_long` as it read there, whitespace removed.  Update both
# together, and only after re-reading the function against the oracle.
CORE_VERSION = "release-0.97.0-66-g9f8cb5ffa"
CORE_DIGEST = "a6c7a1dd59c60453b1ba27ee3befad95d8b1a8b8d2a618d2365a1dc122bc84ac"
CORE_FUNCTION = re.compile(
    r"impure function resolve_address_to_long.*?end resolve_address_to_long;", re.S
)


def check_core(root: str) -> bool:
    """True if the hardware still reads the way the oracle says it does."""
    path = os.path.join(root, "src", "vhdl", "gs4510.vhdl")
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            found = CORE_FUNCTION.search(handle.read())
    except OSError as error:
        print(f"FAIL  cannot read {path}: {error}")
        return False
    if not found:
        print(f"FAIL  no resolve_address_to_long in {path}")
        return False

    digest = hashlib.sha256("".join(found.group(0).split()).encode()).hexdigest()
    if digest != CORE_DIGEST:
        print(
            f"FAIL  resolve_address_to_long has changed since {CORE_VERSION}.\n"
            f"      Re-read it against resolve() below, then update CORE_DIGEST\n"
            f"      and CORE_VERSION.  Digest is now {digest}"
        )
        return False
    print(f"core {CORE_VERSION}: resolve_address_to_long unchanged")
    return True


# The register set the slot gives us, named as the VHDL names them.
State = tuple[int, int, int, int, int, int, int]

# CpuMapMechanism in cpumap.h, which the harness prints as a number.
RAM, PORT, ROM8, ROMA, ROMC, ROME, CRAM2K, MAPLO, MAPHI = range(9)


def resolve(state: State, address: int, writing: bool) -> tuple[int, int]:
    """gs4510.vhdl's resolve_address_to_long, cartridge and hypervisor removed.

    The structure is the point: only the MAP case returns, and everything after
    it overwrites `temp`, so the mechanism appearing latest in the function is
    the one that wins.  `$D030` is applied after the `$01` cases (VHDL ~9253
    against ~9100), which is the ordering most prose gets backwards.
    """
    map_lo, map_hi, mb_lo, mb_hi, port, ddr, d030 = state

    # -- MAP, and it returns (VHDL ~9061).
    block = (address >> 13) & 3
    if address & 0x8000:
        chosen, megabyte = map_hi, mb_hi
    else:
        chosen, megabyte = map_lo, mb_lo
    if (chosen >> 12) & (1 << block):
        return (
            (megabyte << 20) | ((((chosen & 0x0FFF) << 8) + address) & 0xFFFFF),
            MAPHI if address & 0x8000 else MAPLO,
        )

    temp = address
    by = RAM

    # -- $01, with a line driven as input reading as 1 (VHDL ~9085).
    lines = (port | ~ddr) & 0x07
    nibble = address >> 12

    if nibble == 0x0D:
        if lines in (0b101, 0b110, 0b111):
            # All four personalities alias one register set; the slot keeps
            # them at $FFD3xxx.
            temp = 0xFFD3000 | (address & 0x0FFF)
            by = PORT
            # $DE00-$DFFF is the cartridge port unless something claims it
            # first.  Of the four conditions the VHDL tests, only CRAM2K is
            # knowable from a freeze slot: the SD sector buffer mapping lives
            # in the stash at $FFD6000 rather than at $D680, the ethernet
            # personality is not recorded, and Ocean cart mode is a cartridge.
            if address >= 0xDE00 and not (d030 & 0x01):
                temp = 0x7FFD000 | (address & 0x0FFF)
                by = CRAM2K
        elif lines in (0b001, 0b010, 0b011):
            # Character ROM reads at $2D000; writing goes to RAM underneath.
            temp = (0x0000D000 if writing else 0x0002D000) | (address & 0x0FFF)
            by = PORT
        else:
            temp = 0x0000D000 | (address & 0x0FFF)

    if nibble in (0x0E, 0x0F) and (lines & 0b010) and not writing:
        temp, by = 0x20000 + address, PORT
    if nibble in (0x0A, 0x0B) and (lines & 0b011) == 0b011 and not writing:
        temp, by = 0x20000 + address, PORT

    # -- $D030, last and therefore highest among the unmapped mechanisms, and
    # reads only (VHDL ~9253).
    if not writing:
        if nibble in (0x0E, 0x0F) and d030 & 0x80:
            temp, by = 0x20000 + address, ROME
        elif nibble == 0x0C and d030 & 0x20:
            temp, by = 0x20000 + address, ROMC
        elif nibble in (0x0A, 0x0B) and d030 & 0x10:
            temp, by = 0x20000 + address, ROMA
        elif nibble in (0x08, 0x09) and d030 & 0x08:
            temp, by = 0x20000 + address, ROM8

    return temp, by


# Finer than cpumap.c's own walk, on purpose.  An oracle that samples on the
# same grid as the code under test shares its blind spots: at 4KB neither would
# see the cartridge split at $DE00 or a MAP offset wrapping mid-window.
ORACLE_STEP = 0x40


def typed_address(state: State, typed: int) -> int:
    """What the monitor makes of an address as typed.

    Bit 31 asks for the frozen CPU's view; anything else is already a 28-bit
    address.  Both are masked to 28 bits, so neither the flag nor the three bits
    beside it can reach the freeze slot.
    """
    if typed & 0x80000000:
        return resolve(state, typed & 0xFFFF, False)[0] & 0x0FFFFFFF
    return typed & 0x0FFFFFFF


# Typed addresses worth pinning: the flag over each mechanism's window, the same
# addresses unflagged, and the bits beside the flag that must not survive.
TYPED = sorted(
    {0x80000000 | address for address in (0x0000, 0x8000, 0xA000, 0xC000, 0xD020, 0xDE00, 0xE000)}
    | {0x0000, 0xD020, 0x2E000, 0xFFD3020, 0x7FFDE00}
    | {0xF000D020, 0x7000D020, 0x8FFFFFFF}
)


def runs(state: State) -> list[tuple[int, int, int, int]]:
    """The merged runs, coalesced from per-window results rather than by
    repeating the walk under test: start a new run wherever the mechanism
    changes or the destination stops being contiguous."""
    out: list[tuple[int, int, int, int]] = []
    for window in range(0, 0x10000, ORACLE_STEP):
        target, by = resolve(state, window, False)
        if out:
            first, _, run_target, run_by = out[-1]
            if by == run_by and target == run_target + (window - first):
                out[-1] = (first, window + ORACLE_STEP - 1, run_target, run_by)
                continue
        out.append((window, window + ORACLE_STEP - 1, target, by))

    # Independent of the comparison against C: a run claims one unbroken span,
    # so its own last address must resolve where the run says it does.
    for first, last, target, _ in out:
        end, _ = resolve(state, last, False)
        if end != target + (last - first):
            raise AssertionError(
                f"oracle run ${first:04X}-${last:04X} is not contiguous: "
                f"${last:04X} resolves to ${end:X}, not ${target + (last - first):X}"
            )
    return out


# Frozen states worth resolving against, each named so a failure says which
# machine configuration broke rather than which hex blob did.
#            maplo  maphi  mblo  mbhi  port  ddr   d030
STATES: list[tuple[str, State]] = [
    ("bare machine, nothing banked", (0x0000, 0x0000, 0x00, 0x00, 0x00, 0x3F, 0x00)),
    ("C64 defaults $01=$37", (0x0000, 0x0000, 0x00, 0x00, 0x37, 0x3F, 0x00)),
    ("C64 no BASIC $01=$36", (0x0000, 0x0000, 0x00, 0x00, 0x36, 0x3F, 0x00)),
    ("C64 charrom visible $01=$33", (0x0000, 0x0000, 0x00, 0x00, 0x33, 0x3F, 0x00)),
    ("$01 lines floating, DDR all input", (0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00)),
    ("boot $D030=$64, ROMC only", (0x0000, 0x0000, 0x00, 0x00, 0x37, 0x3F, 0x64)),
    ("$D030 all four ROMs", (0x0000, 0x0000, 0x00, 0x00, 0x00, 0x3F, 0xB8)),
    # CRAM2K keeps $DE00-$DFFF on the I/O page; without it those 512 bytes are
    # the cartridge port, which is the default and so the common case.
    ("$D030.0 CRAM2K set", (0x0000, 0x0000, 0x00, 0x00, 0x37, 0x3F, 0x65)),
    # The precedence pairs: both mechanisms want $A000-$BFFF and $E000-$FFFF.
    ("$D030 ROMA with $01 saying RAM", (0x0000, 0x0000, 0x00, 0x00, 0x35, 0x3F, 0x10)),
    ("$D030 ROME with $01 saying RAM", (0x0000, 0x0000, 0x00, 0x00, 0x35, 0x3F, 0x80)),
    ("MAP over everything $D030 wants", (0xF3FF, 0xF3FF, 0x00, 0x00, 0x37, 0x3F, 0xB8)),
    # MAP proper: selection nibbles, offsets, wrap, and the megabyte bytes.
    ("MAPLO block 1 at offset $45200", (0x2452, 0x0000, 0x00, 0x00, 0x37, 0x3F, 0x00)),
    ("MAPHI KERNAL at $30000", (0x0000, 0xB300, 0x00, 0x00, 0x37, 0x3F, 0x00)),
    ("MAPHI negative offset $FA000", (0x0000, 0xFFA0, 0x00, 0x00, 0x37, 0x3F, 0x00)),
    ("MAPHI into attic RAM", (0x0000, 0x2F60, 0x00, 0x80, 0x37, 0x3F, 0x00)),
    ("MAPLO megabyte set, MAPHI not", (0xF000, 0x0000, 0xFF, 0x00, 0x37, 0x3F, 0x00)),
    # The 20-bit sum wraps partway through the mapped half, so a run that looked
    # contiguous at its edges is not: $FF00 lands at $00000, not $100000.
    ("MAPHI offset wrapping mid-run", (0x0000, 0xFF01, 0x00, 0x00, 0x37, 0x3F, 0x00)),
]

# One address in every 4KB window, plus the edges of each mechanism's range and
# the bytes either side of the I/O window.
ADDRESSES = sorted(
    {nibble << 12 for nibble in range(16)}
    | {(nibble << 12) + 0x0FFF for nibble in range(16)}
    | {0x0000, 0x1FFF, 0x2000, 0x7FFF, 0x8000, 0x9FFF, 0xA000, 0xBFFF}
    | {0xCFFF, 0xD000, 0xD020, 0xDBFF, 0xDC00, 0xDDFF, 0xDE00, 0xDFFF, 0xE000, 0xFFFA, 0xFFFF}
    # The Book's own worked example, memory.tex "The MAP Register": $335F
    # through MAPLO $2452 reaches $4855F.  Its arithmetic, not ours.
    | {0x335F}
)


def _script() -> tuple[str, list[tuple[str, str]]]:
    lines: list[str] = []
    checks: list[tuple[str, str]] = []

    for name, state in STATES:
        lines.append("state " + " ".join(f"{value:x}" for value in state))
        for address, writing in itertools.product(ADDRESSES, (False, True)):
            lines.append(f"{'write' if writing else 'read'} {address:x}")
            want, _ = resolve(state, address, writing)
            verb = "writing" if writing else "reading"
            checks.append((f"{name}: {verb} ${address:04X}", f"{want:x}"))

        # The rows the map draws, which must describe the same resolution the
        # per-address checks above pin -- a table that reads plausibly while
        # disagreeing with what M then shows is the failure worth catching.
        for typed in TYPED:
            lines.append(f"typed {typed:x}")
            checks.append((f"{name}: typed ${typed:08X}", f"{typed_address(state, typed):x}"))

        lines.append("runs")
        for first, last, target, by in runs(state):
            checks.append((f"{name}: run from ${first:04X}", f"{first:x} {last:x} {target:x} {by}"))
        checks.append((f"{name}: run list ends", "END"))

    return "\n".join(lines) + "\n", checks


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--core", help="mega65-core checkout, to re-check the oracle against")
    args = ap.parse_args()

    if args.core:
        if not check_core(args.core):
            return 1
    else:
        # Said out loud: without it a stale oracle looks exactly like a good one.
        print("core check skipped, pass --core to compare against gs4510.vhdl")

    script, checks = _script()
    with tempfile.TemporaryDirectory() as tmp:
        # src/ is on the include path already: cpumap.h names Addr28, which
        # lives in src/addr28.h.
        binary = hostbuild.build(
            args.cc, tmp, "cpumap", [os.path.join(HERE, "cpumap_host_harness.c")]
        )
        got = hostbuild.lines(binary, script)

    if len(got) != len(checks):
        print(f"harness produced {len(got)} lines, expected {len(checks)}")
        return 1

    failures = 0
    for (label, want), actual in zip(checks, got, strict=True):
        if actual != want:
            if failures < 20:
                print(f"FAIL  {label}: expected ${want.upper()}, got ${actual.upper()}")
            failures += 1

    if failures:
        print(f"\n{failures} of {len(checks)} checks failed")
        return 1
    print(f"{len(checks)} checks passed across {len(STATES)} frozen machine states")
    return 0


if __name__ == "__main__":
    sys.exit(main())
