#!/usr/bin/env python3
"""Drive a fetch on the machine itself and read what each layer counted.

Xemu has no ethernet on macOS, so a transfer under load can be watched here or
nowhere.  The counters say where frames stop: how many the controller handed
over, how many of those the transfer recognised, and how many carried a block
that had not arrived before.  A failure that would otherwise read as "the
transfer stopped" is placed at one layer by which count falls short.

  eth_rx_rotates     frames taken from the controller, the denominator.
  eth_rx_late        the window had not settled when the frame was read.  The
                     buffer the CPU reads through is registered, so it changes
                     a cycle or more after the write, and etherload.a65 polls
                     before reading where eth_receive() does not.
  eth_rx_norotate    the rotation never happened at all: the controller
                     declines to advance into a buffer the ethernet side still
                     holds, and says so nowhere the receive path looks, so the
                     frame just consumed would be delivered a second time.
  eth_sends          frames put on the wire.  Against the blocks below it says
                     whether repeats are being answered, which is the doubling
                     a client must not do.
  eth_tx_busy        frames written over a transmitter still sending.
  fetch_heard_count  frames that belonged to this transfer, repeats included.
  fetch_block_count  frames that carried a new block.  Steady across
                     duplication rates is the sign repeats are being refused.

Counting starts when the tool is loaded, so a run enters FILEHOST itself rather
than finding it already there.  Needs a build with -DETH_COUNTERS=ON both on
the card and here, the second to put names to addresses.

`--fetch` takes only the catalogue, which lands in memory and leaves the card
alone.  `--image` then fetches one image, which is the transfer long enough to
reach where the failures are, and does write to the card.  Given neither, the
counters of a transfer already run are read and the tool is left on screen.

    cmake -B build-eth -DETH_COUNTERS=ON && cmake --build build-eth
    python3 test/tftp_test_server.py --root /tmp/tftproot --repeat-every 10
    python3 test/verify_ethcount_hw.py --device /dev/cu.usbserial-AQ027F6E \\
            --build build-eth/src --fetch 192.168.68.56:6970 \\
            --image "BLADE RUNNER"
"""

import argparse
import contextlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m65harness as h
import scenario
import verify_filehost_hw

# The same screen verify_filehost_hw watches, and for the same reason.
TOOL_SCREEN = verify_filehost_hw.TOOL_SCREEN

COUNTERS = ("eth_rx_rotates", "eth_rx_late", "eth_rx_norotate",
            "eth_rx_no_free_buffers", "eth_sends", "eth_tx_busy", "fetch_heard_count",
            "fetch_block_count",
            )
# Only in a -DSDCARD_COUNTERS=ON build.  Read when present, since what they
# separate -- sectors written from sectors already holding the wanted bytes --
# is the difference between a fetch that writes and one that mostly compares.
SD_COUNTERS = ("sd_reads", "sd_writes", "sd_writes_skipped", "sd_write_failures",
               "sd_polls_before_read", "sd_polls_before_write", "sd_polls_after_write",
               "sd_slowest_polls", "sd_slowest_sector",
               "sd_frames_worst", "sd_frames_total")

# What an image run can leave on the status line once it has stopped moving.
# `FETCHED` is not among them although it ends a catalogue: it is also the live
# progress line an image draws while it is still arriving, so waiting on it
# would stop at the first block.  A successful image is attached instead.
IMAGE_SETTLED = (*(said for said in verify_filehost_hw.DONE if said != "FETCHED"),
                 "ATTACHED", "CANNOT", "ONLY D81 AND PRG")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="the serial monitor, /dev/cu.usbserial-*")
    parser.add_argument("--build", required=True, help="the -DETH_COUNTERS=ON build's src directory")
    parser.add_argument("--fetch", metavar="ADDRESS",
                        help="fetch the catalogue from this server first, rather than "
                             "reading what an earlier transfer left behind.  Read-only: "
                             "a catalogue lands in memory and the card is not touched")
    parser.add_argument("--image", metavar="TITLE",
                        help="then fetch the first image whose title matches, which is "
                             "the transfer large enough to reach where the failures are. "
                             "This one does write to the card, replacing the image with "
                             "a fresh copy of itself")
    parser.add_argument("--no-counters", action="store_true",
                        help="drive the transfer and read nothing.  For timing a shipping "
                             "build, whose counters do not exist -- without this a build "
                             "missing them fails, which is what a renamed or optimised-away "
                             "counter should do rather than quietly reporting nothing")
    parser.add_argument("--baud", type=int, default=h.MONITOR_BAUD)
    args = parser.parse_args()

    with h.attach(args.device, args.baud, screen=TOOL_SCREEN,
                  symbol=scenario.symbol_reader(args.build)) as machine:
        try:
            if args.fetch:
                fetch_catalog(machine, args.fetch)
            if args.image:
                fetch_image(machine, args.image)
            return 0 if args.no_counters else report(machine)
        except h.Failure as failed:
            return h.report_failure(failed)
        finally:
            # Whichever flag drove the machine, and not `--fetch` alone: a run
            # given only `--image` drives it just as far.  A run given neither
            # only reads, and must leave the tool on screen where it found it.
            if args.fetch or args.image:
                # Back to whatever the machine was doing, as verify_filehost_hw
                # does and for the same reason: a run that leaves the tool on
                # screen is one the next run starts inside.
                with contextlib.suppress(h.Failure):
                    verify_filehost_hw.to_freeze_menu(machine)
                machine.resume()


def fetch_catalog(machine: h.Machine, address: str) -> None:
    """Into the tool, pointed at `address`, and one catalogue fetched.

    The counters start at zero when the tool is loaded, so the tool is entered
    here rather than found already running: counts carried over from an earlier
    visit would be attributed to this transfer.
    """
    verify_filehost_hw.to_freeze_menu(machine)
    machine.press("d")
    machine.wait_until(lambda s: "MEGA65 FILEHOST" in s, timeout=15.0)

    machine.press("t")
    machine.wait_until(lambda s: "NEW TFTP SERVER" in s, timeout=10.0)
    machine.type_text(address)
    machine.press("return")

    machine.press("f")
    began = time.monotonic()
    shot = machine.wait_until(lambda s: any(said in s.text(23) for said in verify_filehost_hw.DONE),
                              timeout=verify_filehost_hw.FETCH_TIMEOUT)
    # A catalogue lands in far memory and never on the card, so what this times
    # is the wire and the client with no SD in it.  Only roughly, though: the
    # status line says FETCHED while bytes are still arriving, so the wait ends
    # at the first progress line rather than at the last block.  The server
    # prints the transfer's real duration; take the figure from there.
    print(f"catalogue: {shot.text(23).strip()}  in {time.monotonic() - began:.1f}s")


def fetch_image(machine: h.Machine, title: str) -> None:
    """The named image fetched whole, however long that takes.

    An image already on the card is offered rather than fetched, so `R` is
    pressed for the copy that is already there: replacing it fetches the same
    800KB over the wire, which is the transfer worth watching.  A title not on
    the card never asks, and the wait below sees the fetch finish instead.
    """
    # Lower case deliberately: the search folds what it is given, and a shifted
    # letter on this charset is a graphic rather than a capital.
    machine.press("/")
    machine.wait_until(lambda s: "FIND:" in s, timeout=10.0)
    machine.type_text(title.lower())
    machine.press("return")

    machine.press("return")
    shot = machine.wait_until(
        lambda s: "ATTACH OR REPLACE" in s.text(23) or any(said in s.text(23) for said in IMAGE_SETTLED),
        timeout=20.0)
    if "ATTACH OR REPLACE" in shot.text(23):
        machine.press("r")

    # `ATTACHED` is still on screen while R starts the replacement fetch.  It
    # becomes a terminal result only after a progress line proves this run
    # moved; otherwise the first wait returns before the first data block.
    progressed = False

    def settled(snapshot: h.Snapshot) -> bool:
        nonlocal progressed
        line = snapshot.text(23)
        progressed = progressed or "FETCHED " in line
        return progressed and any(said in line for said in IMAGE_SETTLED)

    # Long: 800KB is sixteen seconds where the sectors are already right, twice
    # that where they are written, and a transfer that is failing spends the
    # server's whole patience -- see its --tries -- before anything is said.
    began = time.monotonic()
    shot = machine.wait_until(settled, timeout=180.0)
    print(f"image: {shot.text(23).strip()}  in {time.monotonic() - began:.1f}s")


def report(machine: h.Machine) -> int:
    # The harness's own verb rather than a second way of spelling it: `count`
    # resolves the symbol and reads the four bytes, which is what
    # verify_sdcount_xemu.py asks for too.
    wanted = []
    for name in (*COUNTERS, *SD_COUNTERS):
        try:
            machine.address(f"FILEHOST.{name}")
        except (h.Failure, KeyError):
            continue  # not a counting build
        wanted.append(name)
    if not wanted:
        # Asked for by name and absent: a counter that was renamed or optimised
        # away must fail here.  A build with none on purpose says so with
        # --no-counters and never reaches this.
        raise h.Failure("no counters in this build; --no-counters to drive it anyway")
    result = machine.drive([("count", f"FILEHOST.{name}") for name in wanted])
    if not result.ok:
        raise h.Failure(f"could not read the counters: {result.failure}")
    # Keyed by the name the step asked for, which carries the program prefix.
    read = {name: result.counted[f"FILEHOST.{name}"] for name in wanted}

    width = max(len(name) for name in wanted)
    for name in wanted:
        print(f"{name:<{width}}  {read[name]}")

    if "eth_rx_rotates" in read:
        rotates = read["eth_rx_rotates"]
        if rotates == 0:
            # Nothing was received, so the counters say nothing about what
            # happens when something is.  A run reporting zeroes as a clean
            # result is worse than one refusing to report.
            print("\nno frames were received: the tool was loaded after the transfer, "
                  "or the transfer never started")
            return 1
        print(f"\nof {rotates} rotations, {read['eth_rx_late']} were read before the "
              f"window settled and {read['eth_rx_norotate']} never happened")

    if "fetch_heard_count" in read:
        print(f"{read['fetch_heard_count']} were TFTP words and "
              f"{read['fetch_block_count']} advanced a file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
