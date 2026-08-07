#!/usr/bin/env python3
"""Check src/format.c against Python's own formatting, on the host.

The formatting is pure -- every function writes into a buffer the caller owns --
so it needs no MEGA65 and no emulator.  That matters because formatting is what
MEGAINFO prints and nothing else tests it: the tools format hex and compose
their own decimals, so a wrong digit is a wrong screen rather than a build
failure.

Run via CTest, or by hand::

    python3 test/verify_format.py --cc cc
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def build_harness(workdir: Path, host_cc: str) -> Path:
    """Compile format.c plus the host driver with the host compiler."""
    binary = workdir / "format_harness"
    subprocess.run(
        # char is unsigned on llvm-mos but may be signed on the host, so the
        # subscript warning is about the host and not about this code.
        [host_cc, "-std=c2x", "-O1", "-Wall", "-Wno-char-subscripts", "-o", str(binary),
         str(HERE / "format_host_harness.c")],
        check=True,
    )
    return binary


def run(binary: Path, commands: list[str]) -> list[str]:
    out = subprocess.run([str(binary)], input="\n".join(commands) + "\n",
                         capture_output=True, text=True, check=True)
    return out.stdout.splitlines()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True, help="host C compiler")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        binary = build_harness(Path(tmp), args.cc)

        commands: list[str] = []
        expected: list[str] = []

        # One nybble, the digit every hex conversion is built from.
        for v in range(16):
            commands.append(f"nybl {v}")
            expected.append(f"{v:X}")

        # Hex at every width the tools use, over a spread of values.
        values = [0, 1, 9, 10, 15, 16, 255, 256, 4095, 4096, 65535,
                  0x12345, 0xFFFFFF, 0x7FFFFFF]
        for v in values:
            for columns in (1, 2, 4, 7, 8):
                commands.append(f"hex {v} {columns}")
                expected.append(f"{v:0{columns}X}"[-columns:])

        # Five columns, space padded: the freezer's slot and drive numbers.
        for v in [0, 1, 9, 10, 99, 100, 999, 1000, 9999, 10000, 65535]:
            commands.append(f"dec {v}")
            expected.append(f"{v:5d}")

        # The appending forms, which compose MEGAINFO's lines.
        for v in [0, 1, 9, 10, 99, 100, 1000, 2024, 65535]:
            commands.append(f"appenddec {v}")
            expected.append(str(v))
        for v in range(100):
            commands.append(f"appenddec2 {v}")
            expected.append(f"{v:02d}")
        for v in [0, 0x5A, 0xFF, 0x1234]:
            for columns in (2, 4):
                commands.append(f"appendhex {v} {columns}")
                expected.append(f"{v:0{columns}X}"[-columns:])

        got = run(binary, commands)

        if len(got) != len(expected):
            print(f"expected {len(expected)} lines, got {len(got)}")
            return 1

        bad = 0
        for cmd, want, have in zip(commands, expected, got):
            if want != have:
                if bad < 10:
                    print(f"{cmd}: expected {want!r}, got {have!r}")
                bad += 1

        print(f"{len(expected)} cases, {bad} mismatches")
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
