#!/usr/bin/env python3
"""Assemble instructions in the monitor and read them back off the card.

This is the deepest path the emulator can reach: the freezer launches the
monitor, `A` assembles typed text into bytes and writes them to the freeze
slot's sectors, and a later `D` fetches those sectors again and disassembles
them.  The write and the read are separate code, so agreement between what was
typed and what comes back means the bytes really made the round trip.

It needs an SD image because the freezer loads its tools from the card, and
Xemu's own image carries stock MEGA65 binaries -- without this the test would
exercise upstream's monitor rather than ours.  The image is cloned before our
builds are copied in, so the one passed in is never modified.  A copy of
MEGA65.ROM lives in such an image, which is why none is committed here: point
this at your own with -DMEGA65_SDIMG=...

macOS only, because the clone-and-inject step uses hdiutil.  Elsewhere the
test does not exist, in the same way the ROM-walk one does not without a ROM.

    python3 test/verify_monitor.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xemu_keys  # noqa: E402

# Typed after the monitor is up: an address on its own, four instructions, a
# blank line to leave the assembler, then a fresh disassembly of the same
# address.  Each is sent as its own line.
SCRIPT = ["a2100", "inx", "iny", "dex", "eom", "", "d2100"]

# What the re-read has to show.  EOM rather than NOP: $EA is End Of Mapping on
# the 45GS02, so NOP is genuinely not an instruction here.
EXPECTED = [",0002100 E8", ",0002101 C8", ",0002102 CA", ",0002103 EA"]


def inject(base_image: str, work_dir: str, build_dir: str) -> str:
    """Clone the image and copy our .M65 files onto its FAT32 partition."""
    if not shutil.which("hdiutil"):
        sys.exit("hdiutil not found; this test is macOS only")
    clone = os.path.join(work_dir, "card.img")
    # -c asks for a copy-on-write clone, so a 4GB image costs nothing.
    subprocess.run(["cp", "-c", base_image, clone], check=True)

    attached = subprocess.run(
        ["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", clone],
        capture_output=True, text=True, check=True,
    ).stdout
    device = attached.split()[0]
    mount = re.search(r"(/Volumes/\S.*)$", attached, re.M)
    if not mount:
        subprocess.run(["hdiutil", "detach", device], capture_output=True, check=False)
        sys.exit(f"no FAT32 volume in {base_image}")
    try:
        # IOMAP.BIN as well as the tools: the bit editor's names live there, and
        # a card without it looks exactly like a database that knows nothing.
        for name in sorted(os.listdir(build_dir)):
            if name.endswith((".M65", ".BIN")):
                shutil.copy(os.path.join(build_dir, name), mount.group(1).strip())
        subprocess.run(["sync"], check=False)
    finally:
        subprocess.run(["hdiutil", "detach", device], capture_output=True, check=False)
    return clone


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--build", required=True, help="directory holding the .M65 builds")
    parser.add_argument("--boot", type=float, default=16.0)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        card = inject(args.sdimg, tmp, args.build)
        dump = os.path.join(tmp, "screen.txt")
        socket_path = f"/tmp/xemu-monitor-{os.getpid()}.sock"
        proc = subprocess.Popen(
            [args.emulator, "-headless", "-sleepless", "-fastboot", "-testing",
             "-model", "3", "-besure", "-sdimg", card, "-uartmon", socket_path,
             "-prgmode", "64", "-prg", os.path.join(args.build, "FREEZER.M65"),
             "-dumpscreen", dump],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(args.boot)
            sock = xemu_keys.socket.socket(
                xemu_keys.socket.AF_UNIX, xemu_keys.socket.SOCK_STREAM
            )
            sock.settimeout(5)
            sock.connect(socket_path)
            time.sleep(0.3)
            xemu_keys.type_text(sock, "m")          # launch the monitor
            time.sleep(5)
            for line in SCRIPT:
                xemu_keys.type_text(sock, line + "\r")
                time.sleep(2)
            sock.close()
            time.sleep(5)
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            if os.path.exists(socket_path):
                os.unlink(socket_path)

        if not os.path.exists(dump):
            sys.exit("emulator wrote no screen dump")
        with open(dump, encoding="utf-8", errors="replace") as handle:
            screen = handle.read()

    missing = [want for want in EXPECTED if want not in screen]
    if not missing:
        print(f"assembled and read back {len(EXPECTED)} instructions")
        return 0
    sys.stdout.write(f"missing {missing}. Screen was:\n{screen}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
