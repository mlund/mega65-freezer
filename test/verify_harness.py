#!/usr/bin/env python3
"""Drive the step runner against a fake machine, with no emulator.

Everything the harness does between "the machine is up" and "the check came
true" is logic, and on real hardware a mistake in it surfaces only as a step
that timed out -- which reads the same whether the program is wrong, the screen
decode is wrong, or the poll never re-read anything.  A socket that answers the
monitor's `m` from a dictionary separates those: the screen says exactly what
this file says it says, so a failure here is the harness.

    python3 test/verify_harness.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m65harness
from m65harness import Failure, Machine, Screen

# Screen codes: A-Z are $01-$1A, and space is $20.
SCREEN = Screen()


def codes(text: str) -> bytes:
    out = bytearray()
    for char in text.upper():
        out.append(ord(char) - ord("A") + 1 if char.isalpha() else ord(char))
    return bytes(out)


class FakeMachine:
    """Answers `m` and `M` from a memory map, and records what was written.

    The reply format is Xemu's own -- ":AAAAAAAA:" and sixteen bytes of hex,
    one such line for `m` and sixteen for `M` -- because that framing, and the
    assembling of a block out of it, is part of what is under test here.
    """

    def __init__(self, *, wide: bool = True, rows: dict[int, str] | None = None):
        self.cell_bytes = 2 if wide else 1
        self.written: list[bytes] = []
        # Counted in whole screens rather than in requests: one screen is
        # several of them, so a callback keyed on requests would fire part way
        # through the first read and the change it was meant to stage would
        # already be there when the snapshot came back.
        self.snapshots = 0
        self.requests = 0
        self.pending = b""
        self.on_snapshot = None
        self.memory = bytearray(0x10000)
        for row, text in (rows or {}).items():
            self.put_row(row, text)

    def put_row(self, row: int, text: str) -> None:
        base = row * SCREEN.row_bytes
        for column, code in enumerate(codes(text)):
            self.memory[base + column * self.cell_bytes] = code

    def _byte(self, address: int) -> int:
        if address == SCREEN.cell_width_at:
            return 0x01 if self.cell_bytes == 2 else 0x00
        if SCREEN.at <= address < SCREEN.at + len(self.memory):
            return self.memory[address - SCREEN.at]
        return 0

    def sendall(self, data: bytes) -> None:
        line = data.decode().strip()
        if line.startswith("s"):
            self.written.append(data)
            return
        at = int(line[1:], 16)
        self.requests += 1
        if at == SCREEN.cell_width_at:
            self.snapshots += 1
            if self.on_snapshot is not None:
                self.on_snapshot(self)
        for _ in range(16 if line.startswith("M") else 1):
            run = bytes(self._byte(at + i) for i in range(16))
            self.pending += f":{at:08X}:{run.hex().upper()}\n".encode()
            at += 16

    def recv(self, _size: int) -> bytes:
        out, self.pending = self.pending, b""
        return out

    def settimeout(self, _seconds):
        pass

    def close(self):
        pass


def main() -> int:
    failures = []
    checked = 0

    def check(name, got, want):
        nonlocal checked
        checked += 1
        if got != want:
            failures.append(f"{name}: got {got!r}, wanted {want!r}")

    # A 16-bit cell is two bytes, an 8-bit cell one.  A scenario can cross
    # between them, so the width is asked of the machine and not configured;
    # getting it wrong reads every other column and still returns text.
    for wide in (True, False):
        fake = FakeMachine(wide=wide, rows={0: "HELLO", 3: "SECOND ROW"})
        shot = Machine(fake).snapshot()
        check(f"row 0 at {'2' if wide else '1'} bytes per cell", shot.text(0)[:5], "HELLO")
        check(f"row 3 at {'2' if wide else '1'} bytes per cell", shot.text(3)[:10], "SECOND ROW")
        check(f"columns at {'2' if wide else '1'} bytes per cell", shot.columns, 40 if wide else 80)

    # Anything outside letters and punctuation is shown rather than guessed at:
    # a rule glyph must not read as a letter to a check matching on words.
    fake = FakeMachine()
    fake.memory[0] = 0x43
    check("unprintable shows as its code", Machine(fake).snapshot().text(0)[:4], "{43}")

    # The whole screen is what `expect` searches, so a phrase in any row counts.
    fake = FakeMachine(rows={7: "DEEP IN THE SCREEN"})
    check(
        "expect searches every row",
        Machine(fake).drive([("expect", "DEEP IN THE SCREEN", 1.0)]).ok,
        True,
    )

    # A step that never comes true fails with the screen attached.  Without the
    # screen a timeout says only that something did not happen.
    fake = FakeMachine(rows={0: "MENU"})
    result = Machine(fake).drive([("expect", "NEVER APPEARS", 0.3)])
    check("missing text fails", result.ok, False)
    check("failure names the step index", str(result.failure).startswith("step 0 "), True)
    check("failure carries the screen", "MENU" in (result.failure.shown or ""), True)

    # The index is the one that failed, not the count of steps run -- a
    # scenario is read by that number when it breaks.
    fake = FakeMachine(rows={0: "MENU"})
    result = Machine(fake).drive([("expect", "MENU", 1.0), ("key", "m"), ("expect", "GONE", 0.3)])
    check("later step names its own index", str(result.failure).startswith("step 2 "), True)

    # Polling means re-reading: a screen that becomes true while waiting is
    # caught.  A single read at the start would miss this and time out.
    fake = FakeMachine(rows={0: "WAIT"})

    def draw_later(machine):
        if machine.snapshots >= 3:
            machine.put_row(0, "DONE")

    fake.on_snapshot = draw_later
    result = Machine(fake).drive([("expect_row", 0, "DONE", 5.0)])
    check("expect re-reads until true", result.ok, True)
    check("expect polled more than once", fake.snapshots > 2, True)

    # expect_at pins a column, for a readout whose position is the point.
    fake = FakeMachine(rows={5: "    VALUE"})
    check(
        "expect_at pins the column",
        Machine(fake).drive([("expect_at", 5, 4, "VALUE", 1.0)]).ok,
        True,
    )
    check(
        "expect_at rejects the wrong column",
        Machine(fake).drive([("expect_at", 5, 3, "VALUE", 0.3)]).ok,
        False,
    )

    # until_ok polls for the check to come up empty, and hands back the last
    # complaints rather than a bare timeout.  Waiting for the screen merely to
    # change would return the half-drawn frame this stages deliberately.
    fake = FakeMachine(rows={0: "OLD"})

    def in_stages(machine):
        if machine.snapshots >= 5:
            machine.put_row(0, "DONE")
        elif machine.snapshots >= 3:
            machine.put_row(0, "PART")

    fake.on_snapshot = in_stages

    def wants_done(shot):
        return [] if "DONE" in shot.text(0) else [f"row 0 reads {shot.text(0)[:4]!r}"]

    shot, problems = Machine(fake).until_ok(wants_done, 5.0)
    check("until_ok waits past a half-drawn screen", problems, [])
    check("and gives back the screen that satisfied it", "DONE" in shot.text(0), True)

    fake = FakeMachine(rows={0: "NEVER"})
    shot, problems = Machine(fake).until_ok(wants_done, 0.3)
    check("until_ok reports what was still wrong", problems, ["row 0 reads 'NEVE'"])
    check("and the screen it gave up on", "NEVER" in shot.text(0), True)

    # `read` checks memory the screen does not show.
    fake = FakeMachine()
    fake.memory[0x40] = 0x01
    check(
        "read matches",
        Machine(fake).drive([("read", SCREEN.at + 0x40, b"\x01")]).ok,
        True,
    )
    check(
        "read rejects a wrong byte",
        Machine(fake).drive([("read", SCREEN.at + 0x40, b"\x02")]).ok,
        False,
    )

    # `M` answers sixteen lines where `m` answers one, so a screen costs nine
    # requests rather than a hundred and twenty-five.  Each one is two emulated
    # frames, so this is most of what a poll costs.
    fake = FakeMachine(rows={0: "HELLO", 24: "LAST ROW"})
    shot = Machine(fake).snapshot()
    check("a screen is read in blocks", fake.requests, 9)
    check("the first row still decodes", shot.text(0)[:5], "HELLO")
    check("and so does the last", shot.text(24)[:8], "LAST ROW")

    # A short read stays on `m`: an `M` at a hardware register would pull 256
    # bytes of I/O with it, and reading those has side effects.
    fake = FakeMachine()
    Machine(fake).read(SCREEN.at, 1)
    check("a single byte is one line request", fake.requests, 1)

    # A monitor that stops answering ends as a Failure like any other check.
    # The socket's own timeout must be the shorter of the two, or it raises
    # first and drive() -- which catches only Failure -- loses the step index
    # and reports a bare traceback instead.
    check(
        "packet wait is shorter than answer wait",
        m65harness.PACKET_TIMEOUT < m65harness.ANSWER_TIMEOUT,
        True,
    )

    class Mute(FakeMachine):
        """Accepts requests and never answers, as a wedged monitor does."""

        def sendall(self, data: bytes) -> None:
            pass

    silent = Machine(Mute())
    m65harness.ANSWER_TIMEOUT = 0.3
    try:
        result = silent.drive([("expect", "ANYTHING", 1.0)])
        check("a silent monitor fails as a step", result.ok, False)
        check("and names the step", str(result.failure).startswith("step 0 "), True)
        check("and says what it was doing", "no answer reading" in str(result.failure), True)
    finally:
        m65harness.ANSWER_TIMEOUT = 5.0

    # A misspelled verb is an error rather than a step that quietly does
    # nothing, which would otherwise leave a scenario passing on fewer checks
    # than it appears to make.
    result = Machine(FakeMachine()).drive([("expct", "MENU")])
    check("unknown verb fails", result.ok, False)
    check("unknown verb is named", "no such step" in str(result.failure), True)

    # count needs a resolver; without one it must say so rather than read
    # address zero.
    result = Machine(FakeMachine()).drive([("count", "sd_reads", 1)])
    check("count without a resolver fails", result.ok, False)

    fake = FakeMachine()
    fake.memory[0x80:0x84] = b"\x40\x02\x00\x00"
    machine = Machine(fake, symbol=lambda _name: SCREEN.at + 0x80)
    result = machine.drive([("count", "sd_reads", 100)])
    check("count reads the symbol", result.counted.get("sd_reads"), 0x240)
    check("count passes its floor", result.ok, True)
    check(
        "count fails below its floor",
        Machine(fake, symbol=lambda _n: SCREEN.at + 0x80).drive([("count", "sd_reads", 0x241)]).ok,
        False,
    )

    # Typing releases between every key: two of the same character running
    # together are otherwise one press and the second is dropped.
    fake = FakeMachine()
    Machine(fake).type_text("mm")
    check("a repeated character is two presses", len(fake.written), 4)

    # Shift is carried in the modifier slot, not as a separate key.
    fake = FakeMachine()
    Machine(fake).type_text("A")
    check(
        "upper case holds shift",
        fake.written[0],
        f"sffd3615 {m65harness.MATRIX['a']:02x} {m65harness.SHIFT:02x}\n".encode(),
    )

    # A key that has no matrix position is refused rather than sent as
    # something else.
    try:
        Machine(FakeMachine()).press("nosuchkey")
        check("unknown key raises", True, False)
    except Failure as failed:
        check("unknown key raises", "no such key" in str(failed), True)

    # macOS reports the custom 2 Mbaud rate as a number tcsetattr() cannot take
    # back.  Opening the channel must first select a named termios rate, then
    # replace it through IOSSIOSPEED.
    serial_calls = []
    originals = (m65harness.os.open, m65harness.os.close, m65harness.termios.tcgetattr,
                 m65harness.termios.tcsetattr, m65harness.termios.tcflush,
                 m65harness.fcntl.ioctl)
    try:
        m65harness.os.open = lambda *_: 73
        m65harness.os.close = lambda fd: serial_calls.append(("close", fd))
        m65harness.termios.tcgetattr = lambda _fd: [1, 2, 3, 4, 2000000, 2000000, [0] * 32]

        def set_attributes(_fd, _when, attributes):
            if attributes[4] == 2000000 or attributes[5] == 2000000:
                raise m65harness.termios.error(22, "Invalid argument")
            serial_calls.append(("termios", attributes[4], attributes[5]))

        m65harness.termios.tcsetattr = set_attributes
        m65harness.termios.tcflush = lambda *_: None
        m65harness.fcntl.ioctl = lambda _fd, _request, value: serial_calls.append(
            ("speed", m65harness.struct.unpack("I", value)[0]))
        try:
            channel = m65harness.SerialChannel("serial", 2000000)
        except m65harness.termios.error:
            channel = None
        check("a custom serial speed opens again", channel is not None, True)
        check("the ioctl sets 2 Mbaud", ("speed", 2000000) in serial_calls, True)
        if channel is not None:
            channel.close()
    finally:
        (m65harness.os.open, m65harness.os.close, m65harness.termios.tcgetattr,
         m65harness.termios.tcsetattr, m65harness.termios.tcflush,
         m65harness.fcntl.ioctl) = originals

    for failure in failures:
        print(failure)
    if failures:
        print(f"{len(failures)} checks failed")
        return 1
    print(f"{checked} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
