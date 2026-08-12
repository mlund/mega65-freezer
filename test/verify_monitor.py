#!/usr/bin/env python3
"""Assemble in the monitor, read it back off the card, and fold a typed address.

The deepest path the emulator can reach: the freezer launches the monitor, `A`
assembles typed text into bytes and writes them to the freeze slot's sectors,
and a later `D` fetches those sectors again and disassembles them.  The write
and the read are separate code, so agreement between what was typed and what
comes back means the bytes really made the round trip.

It needs an SD image because the freezer loads its tools from the card, and
Xemu's own image carries stock MEGA65 binaries -- without this the test would
exercise upstream's monitor rather than ours.  The image is cloned before our
builds are copied in, so the one passed in is never modified.

EOM rather than NOP: $EA is End Of Mapping on the 45GS02, so NOP is genuinely
not an instruction here.

    python3 test/verify_monitor.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario

STEPS = [
    # Freeze menu -> MONITOR.  Its register header is the first thing that says
    # the tool loaded off the card and started.
    ("key", "m"),
    ("expect", "PC   IRQ  NMI", 30),
    # An address on its own opens the assembler; a blank line leaves it.
    ("type", "a2100\r"),
    ("type", "inx\r"),
    ("type", "iny\r"),
    ("type", "dex\r"),
    ("type", "eom\r"),
    ("type", "\r"),
    # Disassemble the same address: these bytes came back off the card, not out
    # of the assembler's own memory.
    ("type", "d2100\r"),
    ("expect", ",0002100 E8", 30),
    ("expect", ",0002101 C8"),
    ("expect", ",0002102 CA"),
    ("expect", ",0002103 EA"),
    # Bit 31 asks for the frozen CPU's view of an address.  $E000 reaches the
    # KERNAL ROM at 2.E000 with it and bank 0 RAM without, so the two listings
    # showing different addresses is the whole feature: the fold happened, and
    # it did not happen to the address that carried no flag.
    ("type", "m8000e000\r"),
    ("expect", ":002E000", 30),
    ("type", "me000\r"),
    ("expect", ":000E000", 30),
]

if __name__ == "__main__":
    sys.exit(scenario.run(__doc__, "FREEZER.M65", STEPS))
