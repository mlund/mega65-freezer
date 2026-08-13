# SPDX-License-Identifier: MIT OR Apache-2.0
# Copyright (c) 2026 Mikael Lund aka Wombat
#
# Deliberately more permissive than the project around it:
# this file is meant to be copied out and used elsewhere.
"""Drive a MEGA65 program under Xemu, and look at the machine while it runs.

Copy this file into any project that tests MEGA65 code under Xemu; it depends on
nothing but the standard library and nothing in it is specific to one program.
What varies between projects is where the screen lives, what the machine is and
what the tools are called, and those are the `Screen` dataclass and the
arguments to `launch`.

Two primitives carry everything: Xemu's serial monitor, which reads and writes
any 28-bit address while the machine runs, and the core's synthetic key slots at
$D615, which are indistinguishable from someone pressing the key.  Both belong
to the machine rather than to the emulator, so a scenario written here describes
something real hardware could be driven through given a serial transport.

A test is a list of steps:

    args = add_arguments(argparse.ArgumentParser()).parse_args()
    sys.exit(run(args, "TOOL.M65", [
        ("key", "m"),
        ("expect", "READY TO GO"),
        ("type", "d2000"), ("key", "return"),
    ], ready="MAIN MENU"))

or, when the check is more than a phrase on screen, the same primitives by hand:

    with launch(emulator, prg, ready="MAIN MENU") as machine:
        cold = machine.snapshot()
        machine.press("right")
        after = machine.wait_until(lambda s: s.text(7) != cold.text(7))

Nothing here waits a fixed number of seconds for the machine to do something.
`-sleepless` runs flat out and real hardware runs in real time, so a constant
that is generous on one is an intermittent failure on the other; every wait is a
poll against a deadline instead.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time
from collections.abc import Callable, Iterable, Sequence
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass, field

# `m <addr>` answers ":AAAAAAAA:" followed by sixteen bytes as hex, and
# `M <addr>` answers sixteen such lines (xemu/targets/mega65/uart_monitor.c,
# m65mon_dumpmem28; mega65-core/src/monitor/monitor.a65 line 430 for the real
# machine, which reads the upper case form the same way).
_LINE = re.compile(rb":([0-9A-Fa-f]{8}):([0-9A-Fa-f]{32})")
_PER_LINE = 16
_PER_BLOCK = _PER_LINE * 16

# 20ms, the interval matrix65 uses against real hardware.  Longer is not safer:
# keys are released after every press, so an over-long hold only delays the
# release the scanner is waiting for.
KEY_DELAY = 0.02

POLL = 0.1
DEFAULT_TIMEOUT = 15.0
READY_TIMEOUT = 30.0

# How long one `m` may take to answer, and how long one packet may.  The packet
# wait must be the shorter of the two, or the socket gives up before the answer
# does and the Failure below is unreachable.
ANSWER_TIMEOUT = 5.0
PACKET_TIMEOUT = 1.0

SHIFT = 0x0F
NO_KEY = 0x7F

# Matrix position per character -- where the key sits in the keyboard matrix,
# not its character code, so 'm' is $24 rather than $6D.
MATRIX = {
    "\r": 0x01,
    "\n": 0x01,
    "3": 0x08,
    "w": 0x09,
    "a": 0x0A,
    "4": 0x0B,
    "z": 0x0C,
    "s": 0x0D,
    "e": 0x0E,
    "5": 0x10,
    "r": 0x11,
    "d": 0x12,
    "6": 0x13,
    "c": 0x14,
    "f": 0x15,
    "t": 0x16,
    "x": 0x17,
    "7": 0x18,
    "y": 0x19,
    "g": 0x1A,
    "8": 0x1B,
    "b": 0x1C,
    "h": 0x1D,
    "u": 0x1E,
    "v": 0x1F,
    "9": 0x20,
    "i": 0x21,
    "j": 0x22,
    "0": 0x23,
    "m": 0x24,
    "k": 0x25,
    "o": 0x26,
    "n": 0x27,
    "+": 0x28,
    "p": 0x29,
    "l": 0x2A,
    "-": 0x2B,
    ".": 0x2C,
    ":": 0x2D,
    "@": 0x2E,
    ",": 0x2F,
    "}": 0x30,
    "*": 0x31,
    ";": 0x32,
    "=": 0x35,
    "/": 0x37,
    "1": 0x38,
    "_": 0x39,
    "2": 0x3B,
    " ": 0x3C,
    "q": 0x3E,
}

# Characters reached by holding shift over another key.
SHIFTED = {
    "!": "1",
    '"': "2",
    "#": "3",
    "$": "4",
    "%": "5",
    "(": "8",
    ")": "9",
    "?": "/",
    "<": ",",
    ">": ".",
}

# Named keys, for what a character cannot express.
#
# HELP and F9-F14 are absent, and not for want of a position: the matrix is 72
# keys and mega65-core's matrix_to_ascii.vhdl gives 67 as HELP.  Xemu's table
# agrees, and its virtkey() repacks row*8+col into row*16+col, which is how 67
# becomes the $83 in its headers.  But 67 arrives as though it were `A`, and 10
# for `a` does nothing, so something between the register and the key queue is
# not what these tables describe.  Add one here only after measuring it.
#
# No up or left either: both are the shifted form of their opposite on a CBM
# keyboard, and SHIFT in the modifier slot does not reach the key queue.
NAMED = {
    "return": 0x01,
    "f1": 0x04,
    "f3": 0x05,
    "f5": 0x06,
    "f7": 0x03,
    "down": 0x07,
    "right": 0x02,
    "delete": 0x00,
    "stop": 0x3F,
}


class Failure(Exception):
    """A check that did not come true, with the screen as it was."""

    def __init__(self, message: str, shown: str | None = None):
        super().__init__(message)
        self.shown = shown


@dataclass(frozen=True)
class Screen:
    """Where the screen is and how wide a cell is.

    Defaults suit a MEGA65 tool drawing at $B800 in 40 columns.  `cell_width_at`
    is asked of the machine on every read rather than configured per test, since
    one scenario can cross between an 8-bit and a 16-bit character mode -- $D054
    bit 0 says which, so a cell is two bytes or one and a row 40 cells or 80.
    """

    at: int = 0xB800
    rows: int = 25
    row_bytes: int = 80
    cell_width_at: int = 0xFFD3054

    @property
    def size(self) -> int:
        return self.rows * self.row_bytes


# Frozen, so one instance is safe to share as the default everywhere.
DEFAULT_SCREEN = Screen()


@dataclass(frozen=True)
class Snapshot:
    """The screen as it was at one moment, decoded on demand."""

    memory: bytes
    cell_bytes: int
    screen: Screen

    @property
    def columns(self) -> int:
        return self.screen.row_bytes // self.cell_bytes

    def codes(self, row: int) -> list[int]:
        """The screen codes of one row -- C64 ROM charset, so A-Z are $01-$1A."""
        base = row * self.screen.row_bytes
        return [self.memory[base + c * self.cell_bytes] for c in range(self.columns)]

    def attributes(self, row: int) -> list[int]:
        """The high byte of each cell, which in 16-bit mode carries the tile
        source; all zero in 8-bit mode, where a cell has no second byte."""
        if self.cell_bytes < 2:
            return [0] * self.columns
        base = row * self.screen.row_bytes
        return [self.memory[base + c * self.cell_bytes + 1] for c in range(self.columns)]

    def text(self, row: int) -> str:
        """One row as something a check can match a phrase against.  Anything
        outside the letters and punctuation, the graphics a rule is drawn from
        included, shows as `{XX}` rather than silently as a letter."""
        return "".join(_glyph(code) for code in self.codes(row))

    def whole(self) -> str:
        """Every row, one per line."""
        return "\n".join(self.text(row) for row in range(self.screen.rows))

    def __contains__(self, phrase: str) -> bool:
        return phrase in self.whole()


def _glyph(code: int) -> str:
    if 0x01 <= code <= 0x1A:
        return chr(code - 1 + ord("A"))
    if 0x20 <= code <= 0x3F:
        return chr(code)
    if code == 0x00:
        return "@"
    return f"{{{code:02X}}}"


@dataclass
class Result:
    """What a run of steps came to."""

    ok: bool
    steps: int
    failure: Failure | None = None
    counted: dict[str, int] = field(default_factory=dict)


class Machine:
    """A running machine, reachable over its serial monitor.

    Built by `launch`, or around a socket that is already open -- which is how
    the step runner is tested with no emulator behind it.
    """

    def __init__(
        self,
        sock,
        screen: Screen = DEFAULT_SCREEN,
        symbol: Callable[[str], int] | None = None,
    ):
        self._sock = sock
        self.screen = screen
        self._symbol = symbol

    def address(self, name: str) -> int:
        """Where a name lives, through the resolver this machine was given."""
        if self._symbol is None:
            raise Failure(f"cannot look up {name!r}: no symbol resolver was given")
        return self._symbol(name)

    # --- the machine's memory ------------------------------------------------

    def read(self, address: int, length: int) -> bytes:
        """Memory read out of the running machine.

        `M` answers sixteen lines where `m` answers one, and both spell them the
        same way, so anything past a single line is asked for in 256-byte
        blocks: a screen is nine requests rather than a hundred and twenty-five,
        and each request costs two emulated frames.  Both Xemu and the MEGA65's
        own monitor ROM read `M` as sixteen lines, so this stays true of a real
        machine over its serial port.

        Anything a line or shorter still goes through `m`.  That keeps the
        probe of a hardware register to the byte asked for -- an `M` there would
        pull 256 bytes of I/O with it, and reading those has side effects.
        """
        out = bytearray()
        at = address
        while len(out) < length:
            remaining = length - len(out)
            bulk = remaining > _PER_LINE
            span = _PER_BLOCK if bulk else _PER_LINE
            self._sock.sendall(f"{'M' if bulk else 'm'}{at:x}\n".encode())

            deadline = time.time() + ANSWER_TIMEOUT
            seen = b""
            while time.time() < deadline:
                # A monitor that has stopped answering has to end as a Failure
                # like any other check, so the wait for one packet is shorter
                # than the wait for the whole answer -- otherwise the socket
                # raises first and the step it belonged to is never named.
                try:
                    seen += self._sock.recv(4096)
                except TimeoutError:
                    continue
                rows = {int(a, 16): bytes.fromhex(h.decode()) for a, h in _LINE.findall(seen)}
                # Taken by following the addresses rather than by counting
                # lines, so a reply that arrives split across packets, or
                # behind the tail of an earlier one, still assembles in order.
                block, want = bytearray(), at
                while want in rows and len(block) < span:
                    block += rows[want]
                    want += _PER_LINE
                if len(block) >= span:
                    out += block[:span]
                    break
            else:
                raise Failure(f"no answer reading ${at:07X} after {ANSWER_TIMEOUT:g}s")
            at += span
        return bytes(out[:length])

    def write(self, address: int, data: bytes) -> None:
        """Set memory, to reach a state the keyboard cannot.  `s` answers
        nothing, so read it back where it matters."""
        for start in range(0, len(data), _PER_LINE):
            chunk = data[start : start + _PER_LINE]
            spelled = " ".join(f"{b:02x}" for b in chunk)
            self._sock.sendall(f"s{address + start:x} {spelled}\n".encode())
            time.sleep(KEY_DELAY)

    # --- the keyboard --------------------------------------------------------

    def press(self, what: str) -> None:
        """One key, by name for the named keys or by character otherwise."""
        if what in NAMED:
            self._press_code(NAMED[what])
        elif len(what) == 1:
            self.type_text(what)
        else:
            raise Failure(f"no such key {what!r}; named keys are {sorted(NAMED)}")

    def type_text(self, text: str) -> None:
        for char in text:
            modifier = NO_KEY
            if char in SHIFTED:
                char, modifier = SHIFTED[char], SHIFT
            lowered = char.lower()
            if lowered.isalpha() and char.isupper():
                modifier = SHIFT
            if lowered not in MATRIX:
                raise Failure(f"no matrix code for {char!r}")
            # Released between every key rather than once at the end: two of the
            # same character running together are otherwise one continuous
            # press and the second is dropped, so "d2000" arrives as "D20".
            self._press_code(MATRIX[lowered], modifier)

    def _press_code(self, code: int, modifier: int = NO_KEY) -> None:
        self._sock.sendall(f"sffd3615 {code:02x} {modifier:02x}\n".encode())
        time.sleep(KEY_DELAY)
        self._sock.sendall(f"sffd3615 {NO_KEY:02x} {NO_KEY:02x} {NO_KEY:02x}\n".encode())
        time.sleep(KEY_DELAY)

    # --- the screen ----------------------------------------------------------

    def snapshot(self) -> Snapshot:
        """The screen now, at whatever cell width the machine is in."""
        wide = self.read(self.screen.cell_width_at, 1)[0] & 0x01
        memory = self.read(self.screen.at, self.screen.size)
        return Snapshot(memory, 2 if wide else 1, self.screen)

    def wait_until(
        self, wants: Callable[[Snapshot], bool], timeout: float = DEFAULT_TIMEOUT, what: str = ""
    ) -> Snapshot:
        """Re-read the screen until `wants` holds, and return that snapshot."""
        deadline = time.time() + timeout
        shot = self.snapshot()
        while True:
            if wants(shot):
                return shot
            if time.time() >= deadline:
                raise Failure(f"{what or 'still not true'} after {timeout:g}s", shot.whole())
            time.sleep(POLL)
            shot = self.snapshot()

    def until_ok(
        self, check: Callable[[Snapshot], list[str]], timeout: float = DEFAULT_TIMEOUT
    ) -> tuple[Snapshot, list[str]]:
        """Poll until `check` finds nothing wrong; give back the screen and
        whatever it still complained about.

        For a check on a whole screen after a keypress.  Waiting for the screen
        merely to *change* hands back a partly-drawn frame and measures that;
        waiting for the thing about to be asserted cannot.  The complaints come
        back rather than a bare timeout, because "the thumbnail is at (5,13)"
        says more than "it never settled" -- and they are the ones from the
        screen that ran out of time, not from a fresh read taken after it.
        """
        deadline = time.time() + timeout
        while True:
            shot = self.snapshot()
            problems = check(shot)
            if not problems or time.time() >= deadline:
                return shot, problems
            time.sleep(POLL)

    # --- steps ---------------------------------------------------------------

    def drive(self, steps: Sequence[tuple]) -> Result:
        """Run a list of steps, and say which one did not come true.

        Returns rather than printing, so a caller can be a test rather than a
        program.  Verbs: key, type, expect, expect_row, expect_at, read, count.
        A check that needs two screens compared, or a whole screen measured,
        wants `until_ok` and the methods above rather than a step.
        """
        counted: dict[str, int] = {}
        for index, step in enumerate(steps):
            try:
                self._step(step, counted)
            except Failure as failed:
                failed.args = (f"step {index} {step!r}: {failed.args[0]}",)
                return Result(False, len(steps), failed, counted)
        return Result(True, len(steps), None, counted)

    def _step(self, step: tuple, counted: dict[str, int]) -> None:
        verb, rest = step[0], step[1:]

        def timeout(at: int) -> float:
            return rest[at] if len(rest) > at else DEFAULT_TIMEOUT

        if verb == "key":
            self.press(rest[0])
        elif verb == "type":
            self.type_text(rest[0])
        elif verb == "expect":
            self.wait_until(lambda s: rest[0] in s, timeout(1), f"{rest[0]!r} not on screen")
        elif verb == "expect_row":
            row, want = rest[0], rest[1]
            self.wait_until(lambda s: want in s.text(row), timeout(2), f"{want!r} not in row {row}")
        elif verb == "expect_at":
            row, column, want = rest[0], rest[1], rest[2]
            self.wait_until(
                lambda s: s.text(row)[column : column + len(want)] == want,
                timeout(3),
                f"{want!r} not at row {row} column {column}",
            )
        elif verb == "read":
            address, want = rest[0], rest[1]
            got = self.read(address, len(want))
            if got != want:
                raise Failure(f"${address:07X} read {got.hex()}, wanted {want.hex()}")
        elif verb == "count":
            self._count(rest, counted)
        else:
            raise Failure(f"no such step {verb!r}")

    def _count(self, rest: tuple, counted: dict[str, int]) -> None:
        """A counter read out of the running program by symbol name.  What it
        measures is transactions rather than time: an emulated card answers
        instantly, so the count is the part that carries over to hardware."""
        name, least = rest[0], (rest[1] if len(rest) > 1 else None)
        value = int.from_bytes(self.read(self.address(name), 4), "little")
        counted[name] = value
        if least is not None and value < least:
            raise Failure(f"{name} is {value}, wanted at least {least}")


def connect(path: str, timeout: float = READY_TIMEOUT, proc=None, log: str | None = None):
    """The monitor socket, waited for rather than slept at.

    Xemu creates it early -- well under a second on an idle machine -- so a
    fixed wait is wasted on every run, and still too short on a slower one.

    `proc` is watched alongside the socket.  An emulator that died during
    startup never creates the socket at all, and waiting out the deadline for it
    reports a missing socket, which points at this code rather than at the
    reason.  One known cause: several emulators started at once race over a
    single config-template path in the user's preferences directory and the
    losers exit, so a parallel `ctest -j` sees this and a serial run never does.
    """
    deadline = time.time() + timeout
    while True:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(PACKET_TIMEOUT)
        try:
            sock.connect(path)
            return sock
        except OSError:
            sock.close()
        if proc is not None and proc.poll() is not None:
            raise Failure(f"the emulator exited with status {proc.returncode}{_tail(log)}")
        if time.time() >= deadline:
            raise Failure(f"no monitor socket at {path} after {timeout:g}s{_tail(log)}")
        time.sleep(POLL)


def _tail(log: str | None, lines: int = 6) -> str:
    """The last of what the emulator printed, to say why it went."""
    if not log or not os.path.exists(log):
        return ""
    with open(log, encoding="utf-8", errors="replace") as handle:
        said = [line.rstrip() for line in handle if line.strip()]
    return "\n  " + "\n  ".join(said[-lines:]) if said else ""


@contextmanager
def launch(
    emulator: str,
    prg: str,
    *,
    sdimg: str | None = None,
    screen: Screen = DEFAULT_SCREEN,
    ready: str | Callable[[Snapshot], bool] | None = None,
    symbol: Callable[[str], int] | None = None,
    socket_path: str | None = None,
    model: str = "3",
    prgmode: str = "64",
    extra: Iterable[str] | None = None,
    stdout=None,
    timeout: float = READY_TIMEOUT,
):
    """Boot `prg` and yield the machine, shutting the emulator down after.

    `ready` is what says the program has drawn and will not drop a keypress --
    a phrase to wait for on screen, or a predicate.  It matters because a
    scenario usually opens by pressing a key, and a key typed before the program
    is up is simply lost, which reads later as the screen never saying what it
    should.

    `stdout` is where the emulator's output is kept for the caller to read
    afterwards, the hypervisor serial channel included.  Without it the output
    still goes to a file beside the socket, since a startup failure has nowhere
    else to explain itself.

    The socket path must be short.  AF_UNIX caps near 104 characters, so it
    cannot live under a long temporary directory.
    """
    path = socket_path or f"/tmp/xemu-{os.getpid()}.sock"
    argv = [
        emulator,
        "-headless",
        "-sleepless",
        "-fastboot",
        "-testing",
        "-model",
        model,
        "-besure",
        "-uartmon",
        path,
        "-prgmode",
        prgmode,
        "-prg",
        prg,
    ]
    if sdimg:
        argv += ["-sdimg", sdimg]
    if extra:
        argv += list(extra)

    with ExitStack() as closing:
        # Kept even when the caller wants none of it: when the emulator fails
        # during startup, what it printed is the only account of why.  A file
        # rather than a pipe -- the run ends with SIGTERM, and a pipe buffer is
        # lost at that moment.
        mine = stdout is None
        keep = closing.enter_context(open(path + ".log", "w", encoding="utf-8")) if mine else stdout
        proc = subprocess.Popen(argv, stdout=keep, stderr=subprocess.STDOUT)
        sock = None
        try:
            sock = connect(path, timeout, proc, getattr(keep, "name", None))
            machine = Machine(sock, screen=screen, symbol=symbol)
            if ready is not None:
                wants = ready if callable(ready) else (lambda s, r=ready: r in s)
                machine.wait_until(wants, timeout, f"{ready!r} never appeared")
            yield machine
        finally:
            # No grace before the signal, and none is needed: the emulator
            # writes each SD sector straight through to the image file
            # (xemu/targets/mega65/sdcard.c, sdcard_write_block), so nothing a
            # tool wrote is still in a buffer when this arrives.  Waiting for
            # the process below is what makes those writes visible to a caller
            # that reads the image afterwards.
            if sock is not None:
                sock.close()
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=20.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            for leftover in (path, path + ".log" if mine else None):
                if leftover and os.path.exists(leftover):
                    os.unlink(leftover)


def add_arguments(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    """The arguments every emulator test takes."""
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--build", required=True, help="directory holding the builds")
    parser.add_argument("--timeout", type=float, default=READY_TIMEOUT)
    return parser


def report_failure(failed: Failure) -> int:
    """Say what did not come true, and show the screen when there was one."""
    sys.stdout.write(str(failed) + "\n")
    if failed.shown:
        sys.stdout.write("Screen was:\n" + failed.shown + "\n")
    return 1


def report(result: Result) -> int:
    """Print what happened and give back an exit status."""
    for name in sorted(result.counted):
        print(f"{name} = {result.counted[name]}")
    if result.ok:
        print(f"{result.steps} steps passed")
        return 0
    return report_failure(result.failure) if result.failure else 1


def run(args: argparse.Namespace, prg: str, steps: Sequence[tuple], **launch_args) -> int:
    """Boot `prg` from the build directory, run `steps`, and report.

    Argument parsing is left outside so a project can add its own -- where the
    card image is, which build to stage -- and work out what `launch` needs
    before calling.  `add_arguments` supplies the three every project wants.

    The Failure that `launch` itself can raise -- no socket, or the program
    never drew -- is caught here too.  That is the commonest way an emulator
    test fails, and it would otherwise end as a traceback rather than as the
    report, losing the screen that says why.
    """
    try:
        with launch(
            args.emulator, os.path.join(args.build, prg), timeout=args.timeout, **launch_args
        ) as machine:
            return report(machine.drive(steps))
    except Failure as failed:
        return report_failure(failed)
