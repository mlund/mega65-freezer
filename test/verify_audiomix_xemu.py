#!/usr/bin/env python3
"""Launch AUDIOMIX from the freeze menu and reach its advanced view.

A bare `-prg AUDIOMIX.M65` never starts: like MONITOR, dropped straight into
memory cold the BASIC command sits unexecuted at the prompt (confirmed
empirically -- the screen still shows "RUN:" after 55 seconds).  So this goes
through the freezer's own launch path, the same way verify_monitor.py does,
and needs an SD image for the same reason: the freezer loads tools from the
card, and Xemu's own image carries stock MEGA65 binaries.

`A` from the freeze menu launches AUDIOMIX into its simple (dB-bar) view; a
second `A` switches to the advanced (hex matrix) view; a cursor-right moves
the column cursor.  `select_row`/`select_column` reset to 0 on every entry to
advanced mode, so the row-3 readout is deterministically "00" on entry and
"10" (column digit, then row digit) after one cursor-right -- unlike the
audio coefficients themselves, this does not depend on hardware register
state left over from a previous run.

16-bit text mode stores two bytes per cell (character, then a high byte that
is always 0 here), and Xemu's `-dumpscreen` dumps raw screen bytes without
knowing that, wrapped at 40 raw bytes per printed line -- so each 40-column
screen row becomes two dump-file lines of interleaved "char, high-byte" pairs.
_row_text() undoes both: joins the pair and keeps every other character.

    python3 test/verify_audiomix_xemu.py --emulator xmega65 --sdimg card.img \\
            --build build/src
"""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card
import xemu_keys

# Column 8 of row 3 in the advanced view: the header text is static, so it is
# present regardless of cursor position.  Not on row 0-2 or 4+, so this alone
# proves both `a` presses landed (freezer -> AUDIOMIX -> advanced view).
ADVANCED_MARKER = "LFT RGT PH1 PH2 BTL BTR HDL HDR"


def _row_text(lines: list[str], row: int) -> str:
    """Reconstruct one 40-column screen row from two dump-file lines."""
    combined = lines[row * 2] + lines[row * 2 + 1]
    return "".join(combined[i] for i in range(0, len(combined), 2))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    parser.add_argument("--build", required=True, help="directory holding the .M65 builds")
    parser.add_argument("--boot", type=float, default=16.0)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        clone = card.inject(args.sdimg, tmp, args.build)
        dump = os.path.join(tmp, "screen.txt")
        socket_path = f"/tmp/xemu-audiomix-{os.getpid()}.sock"
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
                "-sdimg",
                clone,
                "-uartmon",
                socket_path,
                "-prgmode",
                "64",
                "-prg",
                os.path.join(args.build, "FREEZER.M65"),
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
            xemu_keys.type_text(sock, "a")  # freeze menu -> AUDIOMIX, simple view
            time.sleep(5)
            xemu_keys.type_text(sock, "a")  # simple view -> advanced view
            time.sleep(3)
            xemu_keys.press(sock, xemu_keys.NAMED["right"])  # select_column: 0 -> 1
            time.sleep(2)
            sock.close()
            time.sleep(3)
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
            lines = handle.read().split("\n")

    problems = []
    if len(lines) < 8:
        problems.append(f"screen dump too short ({len(lines)} lines)")
    else:
        row3 = _row_text(lines, 3)
        if ADVANCED_MARKER not in row3:
            problems.append(f"advanced view marker not found in row 3: {row3!r}")
        readout = row3[2:4]
        if readout != "10":
            problems.append(f"row-3 readout after cursor-right was {readout!r}, expected '10'")

    if not problems:
        print("reached AUDIOMIX's advanced view and cursor-right registered")
        return 0

    combined = "\n".join(lines)
    sys.stdout.write("\n".join(problems) + f"\nScreen was:\n{combined}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
