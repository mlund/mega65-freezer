#!/usr/bin/env python3
"""Drive a tool the way a person would, as a list of steps.

This is what the freezer's tests add to m65harness.py: the card.  A tool is
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
proves nothing.  ("expect_no_file", NAME) is the mirror, for a tool that removes
one: the name must be on the card before the run, and afterwards both the entry
and the clusters it held have to be gone.  `count` reads a counter out of the
running tool by name: ("count", "MAKEDISK.sd_reads", 1600) looks the symbol up
in that tool's ELF.

The step vocabulary and the polling are the harness's; see m65harness.py.
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
import m65harness

# The freeze menu's banner, which says the menu has drawn and will not drop the
# first keypress a scenario sends.
READY = "MEGA65 FREEZE MENU"

# Fields of a FAT32 directory entry.  The first cluster is in two pieces.
_ENTRY_SIZE_AT = 28
_ENTRY_CLUSTER_HIGH_AT = 20
_ENTRY_CLUSTER_LOW_AT = 26

# Steps checked against the card once the emulator has closed the image, rather
# than sent to the machine.
_CARD_CHECKS = ("expect_file", "expect_no_file")


def _entry(image: str, name: str) -> bytes | None:
    """A file's directory entry, read independently of whatever wrote it."""
    with open(image, "rb") as handle:
        fs = fat32.FAT32(handle, card.partition_offset(image))
        at, _ = fs.find(fat32.short_name(name))
        if at is None:
            return None
        handle.seek(at)
        return handle.read(32)


def _entry_size(entry: bytes) -> int:
    return struct.unpack_from("<I", entry, _ENTRY_SIZE_AT)[0]


def _entry_cluster(entry: bytes) -> int:
    return (struct.unpack_from("<H", entry, _ENTRY_CLUSTER_HIGH_AT)[0] << 16) | struct.unpack_from(
        "<H", entry, _ENTRY_CLUSTER_LOW_AT
    )[0]


def _cluster_is_free(image: str, cluster: int) -> bool:
    """Whether the FAT has the cluster back.  Asked as well as whether the name
    is gone: a delete that stamps the directory and leaks the chain loses the
    card's free space a file at a time, and the name alone would not show it."""
    with open(image, "rb") as handle:
        fs = fat32.FAT32(handle, card.partition_offset(image))
        return fs.get(cluster) == fat32.FREE


def add_arguments(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    """What every test here takes, so a script with its own flags can add them
    to the same parser rather than building a second one."""
    m65harness.add_arguments(parser)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    return parser


def symbol_reader(build: str):
    """Resolve "TOOL.name" against that tool's ELF, for the `count` step."""

    def resolve(where: str) -> int:
        program, _, name = where.partition(".")
        return elf.symbol(os.path.join(build, f"{program}.M65.elf"), name)

    return resolve


@contextmanager
def staged_card(args, *, with_iomap: bool = True, extras: dict[str, bytes] | None = None):
    """A clone of the card carrying this build.

    The clone outlives the machine, so a check on what a tool wrote can read it
    back after the emulator has closed the image.  `with_iomap` leaves the bit
    editor's database off, which must degrade to an unnamed table rather than
    an error.  `extras` adds files the build does not produce.
    """
    with tempfile.TemporaryDirectory() as tmp:
        yield card.inject(args.sdimg, tmp, args.build, with_iomap=with_iomap, extras=extras), tmp


@contextmanager
def machine_on(args, clone: str, prg: str = "FREEZER.M65", **launch_args):
    """A machine booted off a card this caller has already prepared -- one with
    a file removed, say, which has to happen before the emulator opens it.

    Anything `launch` takes passes through, so a test wanting an extra emulator
    option or the hypervisor serial channel does not have to call it directly
    and lose the symbol resolver on the way.
    """
    launch_args.setdefault("ready", READY)
    with m65harness.launch(
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


def run(description: str, prg: str, steps, *, extras: dict[str, bytes] | None = None) -> int:
    """Stage the build onto a card clone, run `steps` against `prg`, report."""
    args = add_arguments(argparse.ArgumentParser(description=description)).parse_args()
    with staged_card(args, extras=extras) as (clone, _tmp):
        wanted_files = [s[1:] for s in steps if s[0] == "expect_file"]
        for name, _ in wanted_files:
            if _entry(clone, name) is not None:
                sys.exit(f"{name} is already on the card; the test would prove nothing")

        # The mirror: a file has to be there before the run, or a delete that
        # never happened reads exactly like one that worked.  Where its chain
        # begins is taken now, the entry being what says so and about to go.
        doomed = []
        for (name,) in [s[1:] for s in steps if s[0] == "expect_no_file"]:
            entry = _entry(clone, name)
            if entry is None:
                sys.exit(f"{name} is not on the card; deleting it would prove nothing")
            doomed.append((name, _entry_cluster(entry)))

        status = m65harness.run(
            args,
            prg,
            [s for s in steps if s[0] not in _CARD_CHECKS],
            sdimg=clone,
            ready=READY,
            symbol=symbol_reader(args.build),
        )

        if status:
            return status
        problems = []
        for name, size in wanted_files:
            entry = _entry(clone, name)
            if entry is None:
                problems.append(f"{name} is not on the card")
            elif _entry_size(entry) != size:
                problems.append(f"{name} is {_entry_size(entry)} bytes, expected {size}")
        for name, cluster in doomed:
            if _entry(clone, name) is not None:
                problems.append(f"{name} is still on the card")
            elif not _cluster_is_free(clone, cluster):
                problems.append(f"{name} is gone but cluster {cluster} is still taken")

    if problems:
        sys.stdout.write("\n".join(problems) + "\n")
        return 1
    return 0
