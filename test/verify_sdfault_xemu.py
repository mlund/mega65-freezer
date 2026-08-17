#!/usr/bin/env python3
"""A card that will not come ready, and a write that returns anyway.

The wait in sdcard_writesector() used to have no bound: it reset the controller
and reissued the write every half second for as long as the card stayed busy,
which on hardware meant sixty seconds of silence at a single sector and a
machine with nothing left but RESTORE.  It is bounded now, and it reports
what happened rather than resetting the controller under an operation it is
still performing.

No emulator can stage that on its own -- Xemu's card answers at once and is
never busy -- so the refusal is asked for by name instead.  `sd_refuse_ready`
counts down the ready checks to answer "not ready" to.

What this proves is the part that has no other test: that a write which cannot
succeed *returns*.  If the unbounded loop came back, the format below would
never finish and this would fail on the clock rather than on an assertion.

It cannot prove the timing.  Whether the bound is long enough for a real card
part way through an internal erase, and whether the reset is needed at all, are
hardware questions -- see the counters in test/verify_ethcount_hw.py.

Needs both options, the faults to stage the refusal and the counters to see
what it cost:

    cmake -B build-fault -DSDCARD_FAULTS=ON -DSDCARD_COUNTERS=ON
    cmake --build build-fault
    python3 test/verify_sdfault_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build-fault/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

DISK_NAME = "ZZFAULT"

STEPS = [
    ("key", "0"),
    ("expect_row", 2, "- NEW D81 DD IMAGE -"),
    ("key", "down"),
    ("key", "down"),
    ("key", "return"),
    ("expect", "ENTER NAME FOR"),
    ("type", DISK_NAME),
    # Staged after the name and before the writing starts, so the refusal lands
    # on the format rather than on the directory entry the name goes into.
    #
    # One is enough now that a refused write gives up rather than resetting and
    # asking again.
    ("poke", "MAKEDISK.sd_refuse_ready", 1),
    ("key", "return"),
    # The format finishes, which is the assertion.  A write that cannot succeed
    # is one sector of an image nothing has promised is good, and MAKEDISK does
    # not yet carry that up; what matters here is that it came back at all.
    ("expect", "CREATED DISK IMAGE", 180),
    # And a write reported that it could not take the sector, which says the
    # refusal reached the code under test rather than being lost somewhere
    # above it.  Without this the run would pass just as well with the poke
    # doing nothing at all.
    ("count", "MAKEDISK.sd_write_failures", 1),
]

if __name__ == "__main__":
    sys.exit(scenario.run(__doc__, "FREEZER.M65", STEPS))
