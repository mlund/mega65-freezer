#!/usr/bin/env python3
"""Check src/makedisk/layout.c against what the machine actually requires.

The expected values are not read back out of this codebase. The disk geometry
is what hyppo's dos_checkimage measures when it decides an image is a D65 --
"85 tracks x 64 sectors x 2 sides = 5,570,560 bytes" -- and what sdcardio.vhdl
addresses as track x 128. The date and time words are the FAT32 directory
format, and the day of month is what $FFD7113 reports, which iomap.txt calls
"Day of month value (binary coded decimal)", counting from 1.

    python3 test/verify_layout.py --cc cc
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# A D81 is a 1581: 80 tracks, 10 sectors a side. A D65 is the MEGA65 HD format,
# 85 tracks of 64 sectors a side. Both must match hyppo byte for byte, since it
# identifies an image only by its length.
D81_SECTORS = 80 * 10 * 2
D65_SECTORS = 85 * 64 * 2
assert D81_SECTORS * 512 == 819200
assert D65_SECTORS * 512 == 5570560  # hyppo dos.asm: the D65 it will attach

# verify_fat32.py stamps with this same moment; the odd second is deliberate,
# since FAT stores seconds halved.
STAMP = (2019, 3, 7, 14, 5, 47)


def fat_time(hour: int, minute: int, second: int) -> int:
    return (hour << 11) | (minute << 5) | (second >> 1)


def fat_date(year: int, month: int, day: int) -> int:
    return ((year - 1980) << 9) | (month << 5) | day


CASES: list[tuple[str, str, str]] = []


def case(command: str, expected: str, why: str) -> None:
    CASES.append((command, expected, why))


# -- Disk geometry ---------------------------------------------------------
# The image size and the number of sectors formatted have to agree; when they
# were separate expressions, a D65 was allocated 10880 sectors and formatted
# for 5440, leaving the back half of every image as whatever was on the card.
case("geometry 0", f"80 20 {D81_SECTORS} {39 * 20} .D81", "D81 is 80 tracks x 20 sectors")
case("geometry 1", f"85 128 {D65_SECTORS} {39 * 128} .D65", "D65 is 85 tracks x 128 sectors")

# -- BCD ------------------------------------------------------------------
for packed, plain in ((0x00, 0), (0x09, 9), (0x23, 23), (0x59, 59), (0x99, 99)):
    case(f"unbcd {packed}", str(plain), f"${packed:02x} is {plain} in BCD")

# -- RTC decoding ---------------------------------------------------------
# $FFD7113 is "Day of month value (binary coded decimal)" and counts from 1;
# FAT stores the day from 1 as well, so nothing may shift it in between.
# Registers are seconds, minutes, hours, day, month, year, all BCD.
case("rtc 47 05 94 07 03 19", "119 3 7 14 5 47", "24-hour reading, bit 7 set")
case("rtc 00 00 80 01 01 00", "100 1 1 0 0 0", "midnight on 2000-01-01")
case("rtc 59 59 A3 31 12 99", "199 12 31 23 59 59", "last second of 2099")
# 12-hour mode: bit 5 is PM. Noon is 12 PM and midnight is 12 AM, so a bare
# "+12 if PM" turns 12 PM into 24 and leaves 12 AM as 12.
case("rtc 00 00 09 15 06 20", "120 6 15 9 0 0", "9 AM in 12-hour mode")
case("rtc 00 00 22 15 06 20", "120 6 15 14 0 0", "2 PM is 14")
case("rtc 00 00 32 15 06 20", "120 6 15 12 0 0", "12 PM is noon, not 24")
case("rtc 00 00 12 15 06 20", "120 6 15 0 0 0", "12 AM is midnight, not 12")

# -- Timestamps -----------------------------------------------------------
_, mon, mday, hour, minute, sec = STAMP
case(f"time {hour} {minute} {sec}", str(fat_time(hour, minute, sec)), "FAT time word")
# tm_year counts from 1900, so 2019 is 119.
case(f"date 119 {mon} {mday}", str(fat_date(2019, mon, mday)), "FAT date word")
# The RTC reports day of month from 1 and FAT stores it from 1, so nothing may
# shift it. Subtracting one dated every file a day early.
case("date 119 1 1", str(fat_date(2019, 1, 1)), "first of the month stays 1")
case("date 100 12 31", str(fat_date(2000, 12, 31)), "last day of a year")

# -- End of chain ---------------------------------------------------------
# FAT32 ends a chain at $0ffffff8 and above; anything below is a real cluster.
case("eoc 2", "0", "cluster 2 is the root directory, not an end")
case("eoc 268435447", "0", "$0ffffff7 is a valid cluster number")
case("eoc 251658240", "0", "$0f000000 is a valid cluster number")
case("eoc 268435448", "1", "$0ffffff8 is the first end-of-chain marker")
case("eoc 268435455", "1", "$0fffffff ends a chain")

# -- FAT sizing -----------------------------------------------------------
for clusters, sectors in ((0, 0), (1, 1), (128, 1), (129, 2), (200, 2), (256, 2), (257, 3)):
    case(f"fatsectors {clusters}", str(sectors), f"{clusters} entries span {sectors} sectors")

# -- Cluster to sector ----------------------------------------------------
# Cluster 2 is the first data cluster, and the stride is the filesystem's own
# sectors-per-cluster -- not a hardcoded 8.
case("clustersector 1000 2 8", "1000", "cluster 2 starts at the data area")
case("clustersector 1000 3 8", "1008", "8 sectors per cluster")
case("clustersector 1000 3 16", "1016", "16 sectors per cluster must not assume 8")
case("clustersector 1000 4 4", "1008", "4 sectors per cluster must not assume 8")

# -- 8.3 names ------------------------------------------------------------
# The buffer is reused across directory entries and never cleared, so the
# rendering has to terminate the string itself and trim from the right place.
case("nameout TEST    D81", "TEST.D81", "padded stem is trimmed")
case("nameout AB      D65", "AB.D65", "short stem")
case("nameout 12345678D81", "12345678.D81", "full-width stem needs no trim")
case("nameout README     ", "README", "an empty extension drops the dot too")
case("namein TEST.D81", "TEST____D81", "dot expands to padding")
case("namein AB.D65", "AB______D65", "short name pads to 8")
case("namein 12345678.D81", "12345678D81", "full-width name")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        binary = os.path.join(tmp, "layout")
        subprocess.run(
            [
                args.cc,
                "-std=c23",
                "-Wall",
                "-o",
                binary,
                os.path.join(HERE, "layout_host_harness.c"),
            ],
            check=True,
        )
        script = "\n".join(command for command, _, _ in CASES) + "\n"
        got = subprocess.run(
            [binary], input=script, capture_output=True, text=True, check=True
        ).stdout.splitlines()

    failures = 0
    for index, (command, expected, why) in enumerate(CASES):
        actual = got[index] if index < len(got) else "<no output>"
        if actual != expected:
            failures += 1
            print(f"FAIL  {command!r}\n      expected {expected!r}, got {actual!r}\n      {why}")

    if failures:
        print(f"\n{failures} of {len(CASES)} checks failed")
        return 1
    print(f"{len(CASES)} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
