#!/usr/bin/env python3
"""Drive a tool the way a person would, as a list of steps.

This is what the freezer's tests add to xemuharness.py: the card.  A tool is
loaded off the card by the freeze menu, so a test that means to exercise ours
has to stage ours onto a clone of a real image first, and a check on what the
tool wrote has to read that image back afterwards.

A test is the steps and nothing else:

    steps = [
        ("key", "0"),
        ("expect_row", 2, "- NEW D81 DD IMAGE -"),
        ("key", "down"), ("key", "down"), ("key", "return"),
        ("expect", "ENTER NAME FOR"),
        ("type", "ZZTEST"), ("key", "return"),
        ("expect", "CREATED DISK IMAGE", 120),
        ("expect_file", "ZZTEST.D81", 819200),
    ]

`expect_file` is checked after the run, once the image is closed, and its name
must not already be on the card -- a test that finds what it meant to create
proves nothing.  `count` reads a counter out of the running tool by name:
("count", "MAKEDISK.sd_reads", 1600) looks the symbol up in that tool's ELF.

The step vocabulary and the polling are the harness's; see xemuharness.py.
"""

import argparse
import os
import struct
import sys
import tempfile
from contextlib import contextmanager

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import elf
import fat32
import xemuharness

# The freeze menu's banner, which says the menu has drawn and will not drop the
# first keypress a scenario sends.
READY = "MEGA65 FREEZE MENU"

# Offset of the size field in a FAT32 directory entry.
_ENTRY_SIZE_AT = 28


def _entry_size(image: str, name: str) -> int | None:
    """What FAT32 records for a file, read independently of whatever wrote it."""
    with open(image, "rb") as handle:
        fs = fat32.FAT32(handle, card.partition_offset(image))
        at, _ = fs.find(fat32.short_name(name))
        if at is None:
            return None
        handle.seek(at)
        entry = handle.read(32)
    return struct.unpack_from("<I", entry, _ENTRY_SIZE_AT)[0]


def add_arguments(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    """What every test here takes, so a script with its own flags can add them
    to the same parser rather than building a second one."""
    xemuharness.add_arguments(parser)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    return parser


def symbol_reader(build: str):
    """Resolve "TOOL.name" against that tool's ELF, for the `count` step."""

    def resolve(where: str) -> int:
        program, _, name = where.partition(".")
        return elf.symbol(os.path.join(build, f"{program}.M65.elf"), name)

    return resolve


@contextmanager
def staged_card(args, *, with_iomap: bool = True):
    """A clone of the card carrying this build.

    The clone outlives the machine, so a check on what a tool wrote can read it
    back after the emulator has closed the image.  `with_iomap` leaves the bit
    editor's database off, which must degrade to an unnamed table rather than
    an error.
    """
    with tempfile.TemporaryDirectory() as tmp:
        yield card.inject(args.sdimg, tmp, args.build, with_iomap), tmp


@contextmanager
def machine_on(args, clone: str, prg: str = "FREEZER.M65", **launch_args):
    """A machine booted off a card this caller has already prepared -- one with
    a file removed, say, which has to happen before the emulator opens it.

    Anything `launch` takes passes through, so a test wanting an extra emulator
    option or the hypervisor serial channel does not have to call it directly
    and lose the symbol resolver on the way.
    """
    launch_args.setdefault("ready", READY)
    with xemuharness.launch(
        args.emulator,
        os.path.join(args.build, prg),
        sdimg=clone,
        timeout=args.timeout,
        symbol=symbol_reader(args.build),
        **launch_args,
    ) as machine:
        yield machine


@contextmanager
def staged_machine(description: str):
    """A machine booted off a card clone, for a check that is more than a
    phrase on screen -- reading memory, or comparing two screens."""
    args = add_arguments(argparse.ArgumentParser(description=description)).parse_args()
    with staged_card(args) as (clone, _tmp), machine_on(args, clone) as machine:
        yield args, machine


def run(description: str, prg: str, steps) -> int:
    """Stage the build onto a card clone, run `steps` against `prg`, report."""
    args = add_arguments(argparse.ArgumentParser(description=description)).parse_args()
    with staged_card(args) as (clone, _tmp):
        wanted_files = [s[1:] for s in steps if s[0] == "expect_file"]
        for name, _ in wanted_files:
            if _entry_size(clone, name) is not None:
                sys.exit(f"{name} is already on the card; the test would prove nothing")

        status = xemuharness.run(
            args,
            prg,
            [s for s in steps if s[0] != "expect_file"],
            sdimg=clone,
            ready=READY,
            symbol=symbol_reader(args.build),
        )

        if status:
            return status
        problems = []
        for name, size in wanted_files:
            got = _entry_size(clone, name)
            if got is None:
                problems.append(f"{name} is not on the card")
            elif got != size:
                problems.append(f"{name} is {got} bytes, expected {size}")

    if problems:
        sys.stdout.write("\n".join(problems) + "\n")
        return 1
    return 0
