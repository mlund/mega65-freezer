#!/usr/bin/env python3
"""Write a file to the card on the machine itself, and read it back.

The C FAT32 writer -- src/fat32.c, which allocates clusters, builds the chain
and writes the directory entry -- has no other test.  verify_fat32.py checks
test/fat32.py, the Python model the emulator fixtures are built from, so a
green suite says nothing about the code that runs on the machine and can leave
a card unreadable.

sdbatch.prg creates SDBATCH.DAT, writes it single-sector and again in CMD25
batches, and reads each batch back.  Its findings go in the sector after the
data, so fetching the whole file is enough to collect them.

**The stale file is the trap.**  A run that created nothing still leaves the
previous SDBATCH.DAT on the card, and fetching that reads as a pass.  So the
file is deleted first and its absence confirmed; only then is the count
believed.

    python3 test/verify_sdbatch_hw.py --ftp .../mega65_ftp.osx \
                                      --etherload .../etherload.osx \
                                      --prg build/src/sdbatch.prg
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

NAME = "SDBATCH.DAT"
FINDINGS = re.compile(rb"SDBATCH .*VERIFIED (\d+) OF (\d+)")


def ftp(tool: str, *commands: str) -> str:
    args = [tool, "-e", "-y"]
    for command in commands:
        args += ["-c", command]
    args += ["-c", "exit"]
    return subprocess.run(args, capture_output=True, text=True, timeout=300).stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ftp", required=True)
    parser.add_argument("--etherload", required=True)
    parser.add_argument("--prg", required=True)
    parser.add_argument("--settle", type=float, default=30.0,
                        help="seconds to let the run finish before fetching")
    args = parser.parse_args()

    ftp(args.ftp, f"del {NAME}")
    if NAME in ftp(args.ftp, "dir"):
        print(f"{NAME} is still on the card; a stale file would read as a pass")
        return 1

    time.sleep(3)
    load = subprocess.run([args.etherload, "-r", args.prg],
                          capture_output=True, text=True, timeout=120)
    if load.returncode:
        print("etherload failed:", load.stderr.strip()[:200])
        return 1
    time.sleep(args.settle)

    with tempfile.TemporaryDirectory() as into:
        subprocess.run([args.ftp, "-e", "-y", "-c", f"get {NAME}", "-c", "exit"],
                       capture_output=True, text=True, timeout=300, cwd=into)
        got = Path(into) / NAME
        if not got.exists():
            print(f"the run left no {NAME}: the file was never created")
            return 1
        found = FINDINGS.search(got.read_bytes())

    if not found:
        print(f"{NAME} came back with no findings sector")
        return 1
    verified, total = int(found.group(1)), int(found.group(2))
    print(found.group(0).decode("ascii", "replace"))
    if not total or verified != total:
        print(f"only {verified} of {total} batches read back correctly")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
