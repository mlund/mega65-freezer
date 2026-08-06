#!/usr/bin/env python3
"""Check block_move() and block_read() against Python's own semantics.

block_move() is memmove: the destination must end up holding what the source
held *before* the move, whatever the overlap. That is the whole reason the
function chooses a direction, and the case it gets wrong -- a forward copy into
a destination inside the source range -- is silently plausible on hardware,
because the first bytes are right and only the tail is corrupt.

The oracle is a Python bytearray slice assignment, which is memmove. The
chunking, the direction choice and the failure paths are what is under test.

Run via CTest, or directly:

    python3 test/verify_blockmove.py --cc cc
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

TEST = Path(__file__).resolve().parent
SRC = TEST.parent / "src" / "monitor"

MEMORY_SIZE = 2048
CHUNK = 256


def build_harness(workdir: Path, host_cc: str) -> Path:
    """Compile blockmove.c plus the host driver with the host compiler."""
    binary = workdir / "blockmove_host"
    subprocess.run(
        [
            host_cc,
            "-std=c2x",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{SRC}",
            str(TEST / "blockmove_host_harness.c"),
            str(SRC / "blockmove.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    return binary


def run(binary: Path, script: list[str]) -> list[str]:
    result = subprocess.run(
        [str(binary)], input="\n".join(script) + "\n", capture_output=True, text=True, check=True
    )
    return result.stdout.splitlines()


def pattern() -> bytearray:
    """The same fill the harness uses."""
    return bytearray(((i * 7 + 3) ^ (i >> 5)) & 0xFF for i in range(MEMORY_SIZE))


def check_move(binary: Path, cases: list[tuple[int, int, int]]) -> list[str]:
    failures = []
    for frm, to, length in cases:
        expected = pattern()
        expected[to : to + length] = expected[frm : frm + length]

        # Dump a window wide enough to catch a tail the direction choice got
        # wrong, not just the first bytes.
        start = max(0, min(frm, to) - 4)
        end = min(MEMORY_SIZE, max(frm, to) + length + 4)
        out = run(binary, ["pattern", f"move {frm} {to} {length}", f"dump {start} {end - start}"])

        status, dump = out[0], out[1] if len(out) > 1 else ""
        if status != "ok":
            failures.append(f"move {frm}->{to} len {length}: reported {status!r}")
            continue
        got = bytes.fromhex(dump.replace(" ", ""))
        want = bytes(expected[start:end])
        if got != want:
            first = next(i for i in range(len(want)) if got[i] != want[i])
            failures.append(
                f"move {frm}->{to} len {length}: first difference at {start + first} "
                f"(got {got[first]:02x}, want {want[first]:02x})"
            )
    return failures


def check_failures(binary: Path) -> list[str]:
    """Reads and writes off the end must report failure, not wrap or crash."""
    failures = []
    cases = [
        ("source runs off the end", MEMORY_SIZE - 8, 0, 32),
        ("destination runs off the end", 0, MEMORY_SIZE - 8, 32),
        ("source starts past the end", MEMORY_SIZE + 16, 0, 4),
    ]
    for name, frm, to, length in cases:
        out = run(binary, ["pattern", f"move {frm} {to} {length}"])
        if out[0] != "fail":
            failures.append(f"{name}: expected fail, got {out[0]!r}")
    return failures


def check_read(binary: Path) -> list[str]:
    """block_read() clamps to one chunk and stops at the first failed byte."""
    failures = []
    mem = pattern()

    out = run(binary, ["pattern", "read 0 1000"])
    count, _, rest = out[0].partition(" ")
    if int(count) != CHUNK:
        failures.append(f"read of 1000 returned {count}, expected the {CHUNK}-byte chunk cap")
    elif bytes.fromhex(rest.replace(" ", "")) != bytes(mem[0:CHUNK]):
        failures.append("read of 1000 returned the wrong bytes")

    # Straddling the end: only the mapped part arrives.
    want = MEMORY_SIZE - 10
    out = run(binary, ["pattern", f"read {want} 32"])
    count = int(out[0].split(" ", 1)[0])
    if count != 10:
        failures.append(f"read straddling the end returned {count}, expected 10")

    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True, help="host C compiler")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        binary = build_harness(Path(tmp), args.cc)

        cases = [
            # Disjoint, within one chunk and across several.
            (0, 512, 16),
            (0, 512, 300),
            (100, 900, 600),
            # Overlapping upward: the case a forward copy corrupts.
            (100, 110, 64),
            (100, 110, 300),
            (0, 255, 512),
            # Overlapping downward.
            (110, 100, 64),
            (400, 100, 600),
            # Exactly a chunk, and one either side of it.
            (0, 1000, CHUNK - 1),
            (0, 1000, CHUNK),
            (0, 1000, CHUNK + 1),
            # Degenerate.
            (0, 1000, 0),
            (0, 1000, 1),
            (500, 500, 100),
        ]
        failures = check_move(binary, cases)
        failures += check_failures(binary)
        failures += check_read(binary)

    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print(f"blockmove: {len(cases)} moves, failure paths and block_read all agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
