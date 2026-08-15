#!/usr/bin/env python3
"""Fetch the FileHost catalogue over the wire, on the machine itself.

The one thing no emulator run can check.  Xemu has no ethernet on macOS, so
everything the network stack does -- a lease, an address resolved, 512 bytes at
a time off a TFTP server -- is exercised here or nowhere.  The rest of FILEHOST
is covered by verify_filehost_xemu.py, which is faster and needs no hardware.

Read-only, deliberately: nothing is written to the card and nothing is attached
to the frozen machine's drive.  A catalogue fetched over TFTP lands in memory,
so a run leaves the card exactly as it found it, and the frozen program is
resumed at the end whatever happened.

What it needs, none of which this can arrange for itself:

  * the MEGA65 on, running something to freeze, and reachable at --device
  * FILEHOST.M65 on its card, recent enough to have the F key
  * a TFTP server the machine can reach, serving `catalog`, at whatever
    address FILEHOST is set to.  megatalk-tftpd from ether65 serves the real
    FileHost on demand:
        megatalk-tftpd --bind 0.0.0.0:6969 --prefetch

    python3 test/verify_filehost_hw.py --device /dev/cu.usbserial-AQ027F6E
"""

import argparse
import contextlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m65harness as h

# Where the freezer's tools draw, and where BASIC does; the run crosses between
# them, since it starts and ends outside the freezer.
TOOL_SCREEN = h.Screen(at=0xB800)
# Longer than the tool's own budget for a transfer, or a run that is about to
# say why it failed is cut off saying nothing: fetch.c gives up after 1800
# video frames, half a minute.
FETCH_TIMEOUT = 45.0
# What fetch_catalog() can leave on the status line, from main.c's table.
DONE = ("FETCHED", "NO ADDRESS", "NO TFTP SERVER", "DID NOT ANSWER", "REFUSED",
        "STOPPED PART WAY", "MORE THAN THERE IS ROOM FOR", "NOT A CATALOGUE",
        "CATALOGUE CUT SHORT", "THE CATALOGUE IS EMPTY")


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
    args = parser.parse_args()

    with h.attach(args.device, args.baud, screen=TOOL_SCREEN) as machine:
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
    machine.press("s")
    asking = machine.wait_until(lambda s: "NEW TFTP SERVER" in s, timeout=10.0)
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

    return 0


if __name__ == "__main__":
    sys.exit(main())
