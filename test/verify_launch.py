#!/usr/bin/env python3
"""Check that the freezer can find, load and start a tool.

Tools are launched from the freeze menu, which has already set up the VIC
before it hands over.  Dropped straight into memory with `-prg` they never
start -- the BASIC command sits unexecuted at the prompt -- so a cold harness
says nothing about that path.  This boots the freezer, presses the menu key,
and looks for a line the launched tool draws.

What it covers is the *freezer's* side: reading the region list, finding the
file, loading and running it.  The tool that appears is the SD image's, not
this project's build -- Xemu's own image carries stock MEGA65 binaries, and
`-virtsd -sdimg <dir>` is not enough to replace them because a
directory-backed FAT32 has no system partition for the freeze slots.  So a
failure here means the freezer stopped being able to launch anything, not that
our monitor regressed.

    python3 test/verify_launch.py --emulator xmega65 --prg FREEZER.M65 \\
            --keys m --expect "PC   IRQ  NMI"

Deliberately one marker rather than a whole-screen diff: the screen carries
values that move between runs, and what is being tested is that the launch
worked and the tool rendered, not the pixels.
"""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xemu_keys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--prg", required=True, help="the launcher, usually FREEZER.M65")
    parser.add_argument("--keys", required=True, help="characters to type once it is up")
    parser.add_argument("--expect", required=True, help="text the launched tool draws")
    parser.add_argument("--boot", type=float, default=15.0, help="seconds before typing")
    parser.add_argument("--settle", type=float, default=10.0, help="seconds after typing")
    args = parser.parse_args()

    # AF_UNIX caps near 104 characters, so the socket cannot live in a long
    # temporary directory; /tmp keeps it short enough.
    socket_path = f"/tmp/xemu-launch-{os.getpid()}.sock"
    with tempfile.TemporaryDirectory() as tmp:
        dump = os.path.join(tmp, "screen.txt")
        proc = subprocess.Popen(
            [
                args.emulator,
                "-headless",
                "-sleepless",
                "-fastboot",
                "-testing",
                "-model",
                "3",
                "-besure",
                "-uartmon",
                socket_path,
                "-prgmode",
                "64",
                "-prg",
                args.prg,
                "-dumpscreen",
                dump,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(args.boot)
            sock = xemu_keys.socket.socket(xemu_keys.socket.AF_UNIX, xemu_keys.socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect(socket_path)
            time.sleep(0.3)
            xemu_keys.type_text(sock, args.keys)
            sock.close()
            time.sleep(args.settle)
        finally:
            # SIGTERM rather than SIGKILL: the dump is written on the way out.
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

    if args.expect in screen:
        print(f"launched: found {args.expect!r}")
        return 0

    sys.stdout.write(f"did not find {args.expect!r}. Screen was:\n{screen}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
