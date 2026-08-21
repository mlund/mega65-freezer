#!/usr/bin/env python3
"""Fetch the FileHost catalogue over HTTP, on the machine itself.

The one thing no emulator run can check.  Xemu has no ethernet on macOS, so
everything the network stack does -- a lease, an address resolved, and blocks
off a web server -- is exercised here or nowhere.  The rest of FILEHOST is
covered by verify_filehost_xemu.py, which is faster and needs no hardware.

Read-only, deliberately: nothing is written to the card and nothing is attached
to the frozen machine's drive -- except the catalogue itself, which the tool
keeps on the card so the next run needs no network.  The frozen program is
resumed at the end whatever happened.

What it needs, none of which this can arrange for itself:

  * the MEGA65 on, running something to freeze, and reachable at --device
  * FILEHOST.M65 on its card, recent enough to have the F key
  * an ethernet cable and a route out.  No server has to be stood up first:
    the catalogue comes from the public proxy, named in HTTP-IP.TXT on the
    card as an address, a space, and the name to ask it by --

        46.30.215.17 m65filehost.twistedpair.se

    The name is not resolved; it is what the Host header carries, and the
    proxy is name-based virtual hosting, so a bare address earns a 404 on a
    connection that worked perfectly.

    python3 test/verify_filehost_hw.py --device /dev/cu.usbserial-AQ027F6E
"""

import argparse
import contextlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m65harness as h

# Where the freezer's tools draw, and where BASIC does; the run crosses between
# them, since it starts and ends outside the freezer.
TOOL_SCREEN = h.Screen(at=0xB800)
# Longer than the tool's own patience, or a run that is about to say why it
# failed is cut off saying nothing.  fetch.c waits three seconds for a lease and
# three more for an address, then gives up on a transfer once nothing new has
# arrived for eight -- a bound on silence, so a healthy catalogue may take
# longer than any one of them.
FETCH_TIMEOUT = 45.0
# What fetch_catalog() can leave on the status line, from main.c's table.
DONE = ("FETCHED", "NO ADDRESS", "NO PROXY", "DID NOT ANSWER", "REFUSED",
        "STOPPED PART WAY", "MORE THAN THERE IS ROOM FOR", "NOT A CATALOGUE",
        "CATALOGUE CUT SHORT", "THE CATALOGUE IS EMPTY", "CARD WOULD NOT TAKE IT")

# struct FetchCounters in src/filehost/fetch.h, field for field.  Add one there
# and this must change with it: a format string that has drifted still unpacks,
# and reports a transfer of two gigabytes in eight hundredths of a second.
COUNTERS = "<4HI2H"
# Video frames per second, for turning frames into seconds.
FRAMES_PER_SECOND = 50.0


def say_counters(machine: h.Machine) -> None:
    """What the transport had to cope with, read once the fetch is over.

    Once, and afterwards.  A monitor read halts the CPU at an instruction
    boundary and is not subject to the guarantees an interrupt is, so polling
    these while the transfer ran would drop the very frames being counted.
    """
    try:
        at = machine.address("fetch_counters")
    except h.Failure:
        # A shipping build has none: they cost 185 bytes for something only
        # this reads.  Said out loud rather than skipped in silence, so a run
        # that reported no transport numbers does not read like one that
        # measured them and found nothing.
        print("transport counters: absent -- build with -DETH_COUNTERS=ON for them")
        return
    dropped, resent, stalls, heard, moved, frames, first = struct.unpack(
        COUNTERS, machine.read(at, struct.calcsize(COUNTERS)))
    seconds = frames / FRAMES_PER_SECOND
    rate = (moved / 1024.0 / seconds) if seconds else 0.0
    print(f"transport: {moved} bytes in {seconds:.2f}s = {rate:.1f} KB/s, "
          f"first byte after {first / FRAMES_PER_SECOND:.2f}s")
    print(f"           {heard} frames were ours; dropped {dropped}, "
          f"resent {resent}, stalls {stalls}")
    # The number that decides whether receiving strictly in order is enough.
    if dropped:
        print(f"           {dropped} segments fell outside the window -- "
              "reassembly may be worth its lines")


def to_freeze_menu(machine: h.Machine) -> None:
    """The freeze menu, from wherever the machine happens to be.

    A real machine has no known starting state, and a run that failed half way
    leaves one that is anything but: the tool still on screen, its status line
    still saying why.

    Where the machine is comes from the VIC rather than from the screen buffer,
    since a resumed machine still has the freezer's last screen sitting at
    $B800 -- read that and a run decides it is frozen when it is in BASIC, then
    types its keys into whatever is really running.
    """
    if machine.screen_pointer() != TOOL_SCREEN.at:
        machine.freeze()
    elif "FREEZE MENU" not in machine.snapshot():
        machine.press("stop")  # out of a tool, which lands on the menu
    machine.wait_until(lambda s: "FREEZE MENU" in s, timeout=15.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="the serial monitor, /dev/cu.usbserial-*")
    parser.add_argument("--baud", type=int, default=h.MONITOR_BAUD)
    parser.add_argument("--elf", default=None,
                        help="FILEHOST.M65.elf, for reading the transport's counters "
                             "by name rather than by a hand-copied address")
    args = parser.parse_args()

    symbol = None
    if args.elf:
        import elf
        symbol = lambda name: elf.symbol(args.elf, name)  # noqa: E731

    with h.attach(args.device, args.baud, screen=TOOL_SCREEN, symbol=symbol) as machine:
        try:
            return run(machine)
        except h.Failure as failed:
            return h.report_failure(failed)
        finally:
            # Whatever went wrong, the machine goes back to what it was doing:
            # out of the tool first, since F3 means nothing there, and a run
            # that leaves the tool on screen is one the next run starts inside.
            with contextlib.suppress(h.Failure):
                to_freeze_menu(machine)
            machine.resume()


def run(machine: h.Machine) -> int:
    to_freeze_menu(machine)

    machine.press("d")
    machine.wait_until(lambda s: "MEGA65 FILEHOST" in s, timeout=15.0)

    # The address the tool will use, read off its own prompt rather than
    # assumed: a card file or a lease may have said something else.
    machine.press("t")
    asking = machine.wait_until(lambda s: "NEW PROXY" in s, timeout=10.0)
    print("server:", asking.text(23).strip())
    machine.press("stop")

    machine.press("f")
    machine.wait_until(lambda s: any(said in s for said in DONE), timeout=FETCH_TIMEOUT)
    # Read again once it stops moving: the count and the list are drawn a
    # moment apart, and a snapshot taken across that shows one of each.
    shot = machine.settled()
    status = shot.text(23).strip()
    if "FETCHED" not in status:
        print(f"the fetch failed: {status}")
        return 1

    # A catalogue that arrived is one with records in it, and the first row is
    # where the browser draws the first of them.
    first = shot.text(3).strip()
    if not first:
        print("fetched, but the list is empty")
        return 1
    print(f"fetched: {shot.text(0).strip()}")
    print(f"first row: {first[:60]}")
    say_counters(machine)

    return 0


if __name__ == "__main__":
    sys.exit(main())
