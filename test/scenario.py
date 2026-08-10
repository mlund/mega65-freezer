"""Drive a tool the way a person would, as a list of steps.

A test here is a scenario -- press this, wait for that to appear, check the
card afterwards -- and the scenario is the only part worth reading.  Everything
around it is the same every time: parse the arguments, clone the card, write
the build onto it, start the machine, tear it down, and say which check failed
and what was on screen when it did.

So a test is a list:

    steps = [
        ("key", "0"),
        ("expect_row", 2, "- NEW D81 DD IMAGE -"),
        ("key", "down"), ("key", "down"), ("key", "return"),
        ("expect", "ENTER NAME FOR"),
        ("type", "ZZTEST"), ("key", "return"),
        ("expect", "CREATED DISK IMAGE", 120),
        ("expect_file", "ZZTEST.D81", 819200),
    ]

Every `expect` polls rather than sleeping, re-reading the screen until the text
appears or the timeout runs out.  That is what keeps a scenario honest on a
machine of a different speed: `-sleepless` runs flat out and hardware runs in
real time, so a fixed wait is either wasted or a flake.

Both primitives a step uses are the machine's own -- the synthetic keyboard at
$FFD3615 and the serial monitor's memory read -- rather than emulator hooks, so
a scenario describes something a real MEGA65 could be driven through too, given
a transport that speaks to its serial port.
"""

import argparse
import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import fat32
import screen
import xemu
import xemu_keys

DEFAULT_TIMEOUT = 15.0
POLL = 0.5

# Offset of the size field in a FAT32 directory entry.
_ENTRY_SIZE_AT = 28


class Failure(Exception):
    """A step that did not come true, with the screen as it was."""

    def __init__(self, message: str, shown: str | None = None):
        super().__init__(message)
        self.shown = shown


def _press(sock, what: str) -> None:
    if what in xemu_keys.NAMED:
        xemu_keys.press(sock, xemu_keys.NAMED[what])
    elif len(what) == 1:
        xemu_keys.type_text(sock, what)
    else:
        raise Failure(f"no such key {what!r}; named keys are {sorted(xemu_keys.NAMED)}")


def _read_screen(sock) -> bytes:
    return xemu.read(sock, screen.SCREEN_AT, screen.SCREEN_BYTES)


def _poll(sock, wants, timeout: float):
    """Re-read the screen until `wants(memory)` is true, or give up."""
    deadline = time.time() + timeout
    memory = _read_screen(sock)
    while True:
        if wants(memory):
            return memory
        if time.time() >= deadline:
            raise Failure(
                f"still not true after {timeout:g}s",
                screen.screen(memory, screen.SCREEN_AT),
            )
        time.sleep(POLL)
        memory = _read_screen(sock)


def _run_steps(sock, steps, seen: dict) -> None:
    for index, step in enumerate(steps):
        verb, rest = step[0], step[1:]
        try:
            if verb == "key":
                _press(sock, rest[0])
            elif verb == "type":
                xemu_keys.type_text(sock, rest[0])
            elif verb == "wait":
                time.sleep(rest[0])
            elif verb == "expect":
                text, timeout = rest[0], (rest[1] if len(rest) > 1 else DEFAULT_TIMEOUT)
                _poll(sock, lambda m, t=text: t in screen.screen(m, screen.SCREEN_AT), timeout)
            elif verb == "expect_row":
                row, text = rest[0], rest[1]
                timeout = rest[2] if len(rest) > 2 else DEFAULT_TIMEOUT
                _poll(
                    sock,
                    lambda m, r=row, t=text: t in screen.text(m, r, screen.SCREEN_AT),
                    timeout,
                )
            elif verb == "snapshot":
                seen[rest[0]] = _read_screen(sock)
            elif verb == "expect_changed":
                name, row = rest[0], rest[1]
                timeout = rest[2] if len(rest) > 2 else DEFAULT_TIMEOUT
                was = screen.text(seen[name], row, screen.SCREEN_AT)
                _poll(
                    sock,
                    lambda m, r=row, w=was: screen.text(m, r, screen.SCREEN_AT) != w,
                    timeout,
                )
            elif verb == "expect_file":
                pass  # checked after the run, when the image is closed
            else:
                raise Failure(f"no such step {verb!r}")
        except Failure as failed:
            failed.args = (f"step {index} {step!r}: {failed.args[0]}",)
            raise


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


def run(description: str, prg: str, steps, *, boot: float = 16.0) -> int:
    """Parse the usual arguments, run `steps` against `prg`, and report."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--build", required=True, help="directory holding the .M65 builds")
    parser.add_argument("--boot", type=float, default=boot)
    args = parser.parse_args()

    seen: dict = {}
    failure: list[Failure] = []

    def drive(sock):
        try:
            _run_steps(sock, steps, seen)
        except Failure as failed:
            failure.append(failed)

    with tempfile.TemporaryDirectory() as tmp:
        clone = card.inject(args.sdimg, tmp, args.build)

        wanted_files = [s[1:] for s in steps if s[0] == "expect_file"]
        for name, _ in wanted_files:
            if _entry_size(clone, name) is not None:
                sys.exit(f"{name} is already on the card; the test would prove nothing")

        xemu.launch(
            args.emulator,
            os.path.join(args.build, prg),
            sdimg=clone,
            socket_path=f"/tmp/xemu-{os.getpid()}.sock",
            drive=drive,
            boot=args.boot,
        )

        if not failure:
            for name, size in wanted_files:
                got = _entry_size(clone, name)
                if got is None:
                    failure.append(Failure(f"{name} is not on the card"))
                elif got != size:
                    failure.append(Failure(f"{name} is {got} bytes, expected {size}"))

    if not failure:
        print(f"{len(steps)} steps passed")
        return 0

    first = failure[0]
    sys.stdout.write(str(first) + "\n")
    if first.shown:
        sys.stdout.write("Screen was:\n" + first.shown + "\n")
    return 1
