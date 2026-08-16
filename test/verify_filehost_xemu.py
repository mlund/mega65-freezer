#!/usr/bin/env python3
"""Browse the FileHost catalogue, fetch with no network, and attach an image.

Everything the tool does when the wire is not there, which is a case worth
holding still: start-up fetches and fails, so the list is the card's copy, and
attaching an image the card has not got fetches and fails too.  The network path
that works is checked on hardware by verify_filehost_hw.py.

The rest of it: the freeze menu launches it, it
reads a catalogue off the card, renders records the machine has never seen, and
attaches the image one of them names.  The catalogue is written by
test/filehost_catalog.py, which shares no code with the decoder under test.

Three things here are only visible through a screen dump.  Titles arrive as
mixed-case ASCII and the screen is uppercase-only PETSCII, so "Attack of the
Robots" rendering as letters at all is the conversion working.  Choosing a
program rather than a disk image has to say so instead of failing quietly.  And
the mount is only real if the freeze menu's own drive row shows it afterwards --
that row is drawn from the frozen process descriptor, which is what the unfreeze
path reattaches from, so it is the thing that proves the attach will survive a
resume rather than merely having happened.

819200 is not ours to choose: hyppo's dos_checkimage refuses to attach a D81
that is not exactly that size.

    python3 test/verify_filehost_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import filehost_catalog as fhc
import scenario

# The name the browser derives from the first record's path, and so the file
# the attach will look for.  Derived rather than written out: the machine works
# it out from the path, and the point of staging it under the same name is that
# the two derivations agree.
IMAGE_PATH = "files/t/Attack_of_the_Robots_x1.d81"
IMAGE = fhc.catalog_short_name(IMAGE_PATH, fhc.D81)

CATALOG = fhc.catalogue(
    [
        # Mixed case, so the render has to fold it.
        fhc.record("Attack of the Robots", "Somebody", IMAGE_PATH, fhc.D81, 819200,
                   fhc.GAME, 2021),
        fhc.record("Hello World", "Nobody", "files/h/hello.prg", fhc.PRG, 4096,
                   fhc.APPLICATION, 2024),
        # Named by the catalogue and not staged, so attaching it is a fetch.
        # Which code hyppo gives for a name it never found depends on whose
        # hyppo: this emulator says file-not-found and the machine says
        # end-of-directory, and both have to read as "fetch it" -- only one of
        # the two is exercised here.
        fhc.record("Not Fetched Yet", "Nobody", "files/n/not_fetched.d81", fhc.D81, 819200,
                   fhc.DEMO, 2022),
    ]
)

# The first list row, and the two rows the fixture fills.  LIST_TOP_Y in
# src/filehost/browser.h.
FIRST = 3

STEPS = [
    ("key", "d"),
    ("expect_row", FIRST, "ATTACK OF THE ROBOTS", 30),
    ("expect_row", FIRST + 1, "HELLO WORLD"),
    ("expect_row", FIRST, "SOMEBODY"),
    # The columns, so a heading and its values cannot drift apart unnoticed.
    # browser.h places these: TITLE 0, AUTHOR 41, CATEGORY 58, YEAR 70, TYPE 75.
    ("expect_at", FIRST, 58, "GAME"),
    ("expect_at", FIRST + 1, 58, "APPLICATION"),
    ("expect_at", FIRST, 70, "2021"),
    ("expect_at", FIRST, 75, "D81"),
    ("expect_at", FIRST + 1, 75, "PRG"),
    # Sorting.  Newest first puts the 2024 program above the 2021 game, which
    # is the whole of the evidence: no message is printed, because the list is
    # the answer.  Then category, where APPLICATION precedes DEMO and GAME by
    # the enumeration's own order, and a third press is back to title order.
    ("key", "s"),
    ("expect_row", FIRST, "HELLO WORLD"),
    ("key", "s"),
    ("expect_row", FIRST, "HELLO WORLD"),
    ("expect_at", FIRST, 58, "APPLICATION"),
    ("expect_at", FIRST + 1, 58, "DEMO"),
    ("key", "s"),
    ("expect_row", FIRST, "ATTACK OF THE ROBOTS"),
    # Searching, which hides rather than jumps: one row left, and the count
    # says how many of how many so a filter in force cannot be forgotten.
    ("key", "/"),
    ("type", "HELLO"),
    ("key", "return"),
    ("expect_row", FIRST, "HELLO WORLD"),
    ("expect_row", FIRST + 1, ""),
    ("expect", "1 OF 3"),
    # Case-insensitively, and anywhere in the title rather than at its start.
    ("key", "/"),
    ("type", "robots"),
    ("key", "return"),
    ("expect_row", FIRST, "ATTACK OF THE ROBOTS"),
    # Nothing matching says so rather than showing an empty list in silence.
    ("key", "/"),
    ("type", "ZZZZ"),
    ("key", "return"),
    ("expect", "NOTHING MATCHES THAT"),
    # An empty search is how the whole list comes back.
    ("key", "/"),
    ("key", "return"),
    ("expect_row", FIRST + 2, "NOT FETCHED YET"),
    # Down onto the program and in.  The refusal is also what proves the
    # selection moved, which no screen dump shows directly -- the highlight is
    # a colour, and colour RAM is not in the dump.
    ("key", "down"),
    ("key", "return"),
    ("expect", "ONLY DISK IMAGES"),
    # An image the card has not got is fetched rather than refused, so what
    # comes back here is the network's answer -- and Xemu has no network.
    ("key", "down"),
    ("key", "return"),
    # Both halves on one line: why it failed, and that nothing half-written was
    # left behind to be mounted as a working image later.
    ("expect", "THE FILE WAS REMOVED", 60),
    # A fetch with no network at all.  Xemu has no ethernet, so nothing answers
    # the lease and the tool has to say so and put the card's catalogue back
    # rather than sit waiting -- the one part of the network path that can be
    # tested without a wire, and the part a user meets most often.
    # Seen starting before its failure means anything: start-up has already
    # left the same message on the status line, so without this the step passes
    # on the previous screen even if the key did nothing.
    ("key", "f"),
    ("expect", "FETCHING THE CATALOGUE"),
    ("expect", "NO ADDRESS: NOTHING ANSWERED ON THE NETWORK", 30),
    # The same fetch, given up on by hand.  A key reaches the machine whenever
    # it is sent, so this one lands while the lease is still being waited for --
    # which is the only part of a fetch an emulator with no ethernet ever gets
    # to, and the part that proves the tool reads the keyboard at all while it
    # is on the wire.  Without it, a transfer that will never finish can only be
    # escaped by resetting the machine.
    # Said in full, because "STOPPED" alone is also inside the message for a
    # transfer that died on its own -- and this must not pass for that.
    ("key", "f"),
    ("key", "stop"),
    ("expect", "THE FETCH WAS STOPPED", 30),
    # And an address typed in, since a household router names no TFTP server.
    # The field opens empty with the one in force named in the prompt, so what
    # the prompt says the second time is the only proof from outside that what
    # was typed the first time was kept rather than merely echoed.
    ("key", "t"),
    ("expect", "NOW 192.168.68.57:6969"),
    # With a port, since a gateway on one of its own needs no root to run.
    ("type", "10.1.2.3:6969"),
    ("key", "return"),
    ("expect", "SERVER SET"),
    ("key", "t"),
    ("expect", "NOW 10.1.2.3:6969"),
    ("type", "NOT.AN.ADDRESS"),
    ("key", "return"),
    ("expect", "THAT IS NOT AN ADDRESS"),
    # Back to the top.  Not cursor-up: that is SHIFT plus cursor-down on this
    # keyboard, and the modifier does not reach the key queue.
    ("key", ","),
    ("key", "return"),
    # The card already holds this one, so the choice is offered rather than
    # taken: a fetched image can be attached, or replaced when what arrived was
    # rubbish, which is the only repair a machine without a PC beside it has.
    ("expect", "ATTACH OR REPLACE"),
    ("key", "a"),
    ("expect", f"ATTACHED {IMAGE}", 30),
    # Out, and the freeze menu's drive row has to agree.
    ("key", "stop"),
    ("expect", "MEGA65 FREEZE MENU", 30),
    ("expect_row", 21, IMAGE),
    # Last, because it destroys what the steps above needed: replacing writes
    # over the file and, with no network, the fetch that follows fails -- so
    # what this proves is the removal, on a file that really was there.
    ("key", "d"),
    ("expect_row", FIRST, "ATTACK OF THE ROBOTS", 30),
    ("key", "return"),
    ("expect", "ATTACH OR REPLACE"),
    ("key", "r"),
    ("expect", "THE FILE WAS REMOVED", 60),
    ("expect_no_file", IMAGE),
]

if __name__ == "__main__":
    sys.exit(
        scenario.run(
            __doc__,
            "FREEZER.M65",
            STEPS,
            extras={"CATALOG.M65": CATALOG, IMAGE: bytes(819200)},
        )
    )
