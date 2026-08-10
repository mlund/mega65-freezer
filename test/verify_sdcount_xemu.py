#!/usr/bin/env python3
"""Count the SD transactions creating a D81 costs.

The emulator settles nothing about speed: it has no card and answers in no
time.  The count is the half that carries to hardware, to be multiplied there
by a cost a real card can be timed for.  So a change that lowers these numbers
is faster on the machine; one that leaves them alone is not, however much it
moves the emulator.

sd_writes_skipped is the number worth watching.  It counts sectors already
holding what was about to be written, and it decides which of two very
different costs a format pays -- the read-before-write is a large saving when
it hits and a wasted read when it misses.  A card whose freed clusters still
hold old data misses every time, which no emulator will show.

Runs verify_makedisk_xemu.py's scenario and reads the counters when MAKEDISK
reports it is done.  Needs -DSDCARD_COUNTERS=ON.

    cmake -B build-count -DSDCARD_COUNTERS=ON && cmake --build build-count
    python3 test/verify_sdcount_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build-count/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario
import verify_makedisk_xemu

# A D81 is 80 tracks of 20 sectors, so the format loop cannot read fewer than
# 1600 times: one per sector before deciding whether to write it.  Any less and
# it did not format the whole image.
D81_SECTORS = 80 * 20

STEPS = [
    *verify_makedisk_xemu.STEPS,
    ("count", "MAKEDISK.sd_reads", D81_SECTORS),
    ("count", "MAKEDISK.sd_writes"),
    ("count", "MAKEDISK.sd_writes_skipped"),
]

if __name__ == "__main__":
    sys.exit(scenario.run(__doc__, "FREEZER.M65", STEPS))
