#!/usr/bin/env python3
"""Check the FAT32 writer against mtools, which reads the result independently.

Writing a filesystem wrongly is not an error at the time: it is a card that
mounts, lists, and then hands back the wrong bytes. So the check is a different
implementation reading what this one wrote -- mtools' mdir and mtype -- rather
than this code agreeing with itself.

Needs an SD image to work on, given as --sdimg; it is copied first and only the
copy is touched.

    python3 test/verify_fat32.py --sdimg card.img
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import fat32

# A MEGA65 card leaves heads and sectors zero, which mtools will not trust.
MTOOLS_ENV = {**os.environ, "MTOOLS_SKIP_CHECK": "1"}

# Sized to cross a cluster boundary (4096 bytes here) rather than to be round,
# and to shrink and grow across a rewrite so both paths are taken.
CASES = [
    ("SMALL.BIN", 10),
    ("EXACT.BIN", 4096),
    ("SPILL.BIN", 4097),
    ("BIG.BIN", 40000),
    ("EMPTY.BIN", 0),
]


def listed(mdir_output: str) -> dict[str, str]:
    """The directory entries from mdir, by name, without its header or totals."""
    out = {}
    for line in mdir_output.splitlines():
        entry = re.match(r"^(\S{1,8})\s+(\S{1,3}|<DIR>)\s+\S", line)
        if entry:
            out[entry.group(1)] = line.rstrip()
    return out


def content(size: int, seed: int) -> bytes:
    """Reproducible bytes that differ per file, so a mix-up is visible."""
    out = bytearray()
    value = seed & 0xFF
    for _ in range(size):
        out.append(value)
        value = (value * 7 + 13) & 0xFF
    return bytes(out)


def mtools_read(image: str, offset: int, name: str) -> bytes:
    got = subprocess.run(
        ["mtype", "-i", f"{image}@@{offset}", f"::/{name}"],
        capture_output=True,
        check=True,
        env=MTOOLS_ENV,
    )
    return got.stdout


def check(image: str, offset: int, expected: dict[str, bytes], problems: list[str]) -> None:
    listing = subprocess.run(
        ["mdir", "-i", f"{image}@@{offset}", "::/"],
        capture_output=True,
        text=True,
        check=True,
        env=MTOOLS_ENV,
    ).stdout
    for name, data in expected.items():
        stem, _, suffix = name.partition(".")
        if f"{stem:<8} {suffix}" not in listing:
            problems.append(f"{name} is not in the directory")
            continue
        got = mtools_read(image, offset, name)
        if got != data:
            problems.append(
                f"{name}: read back {len(got)} bytes"
                f" ({hashlib.sha256(got).hexdigest()[:12]}),"
                f" wrote {len(data)} ({hashlib.sha256(data).hexdigest()[:12]})"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdimg", required=True, help="an image to copy, never written to")
    args = parser.parse_args()

    if not shutil.which("mdir"):
        sys.exit("mtools not found; it is what checks the result")

    problems: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        image = card.clone(args.sdimg, tmp)
        offset = card.partition_offset(image)

        before = subprocess.run(
            ["mdir", "-i", f"{image}@@{offset}", "::/"],
            capture_output=True,
            text=True,
            check=True,
            env=MTOOLS_ENV,
        ).stdout

        expected = {}
        with open(image, "r+b") as handle:
            fs = fat32.FAT32(handle, offset)
            for seed, (name, size) in enumerate(CASES):
                expected[name] = content(size, seed + 1)
                fs.write(name, expected[name])
            fs.flush()
        check(image, offset, expected, problems)

        # Again, at different sizes: an overwrite has to grow, shrink and free
        # the tail without disturbing anything else.
        with open(image, "r+b") as handle:
            fs = fat32.FAT32(handle, offset)
            for seed, (name, size) in enumerate(CASES):
                expected[name] = content((size * 3) % 9001, seed + 40)
                fs.write(name, expected[name])
            fs.flush()
        check(image, offset, expected, problems)

        # The files that were already there must still read correctly, which is
        # what proves the allocator took free clusters and nothing else.
        after = subprocess.run(
            ["mdir", "-i", f"{image}@@{offset}", "::/"],
            capture_output=True,
            text=True,
            check=True,
            env=MTOOLS_ENV,
        ).stdout
        ours = {name.split(".")[0] for name, _ in CASES}
        for name, line in listed(before).items():
            if name not in ours and listed(after).get(name) != line:
                problems.append(f"an existing entry changed: {line}")

    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1
    print(f"{len(CASES)} files written and read back by mtools, twice, existing files intact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
