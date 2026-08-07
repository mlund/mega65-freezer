#!/usr/bin/env python3
"""Type at a running MEGA65, or at Xemu, over the serial monitor.

The core has three synthetic key slots -- VIRTKEY1/2/3 at $D615/$D616/$D617,
where $7F means no key is down.  Writing a matrix code there is indistinguishable
from someone pressing the key, so this drives the tools the way a person does
rather than starting them cold.

Xemu exposes the monitor on a unix socket with `-uartmon <path>`, and the same
`s` command works against real hardware over the serial port.  Note the socket
path must be short: AF_UNIX caps around 104 characters, so it cannot live under
a long temporary directory.

    xmega65 -headless -sleepless -testing -besure -uartmon /tmp/xemu.sock \\
            -prgmode 64 -prg FREEZER.M65 &
    python3 test/xemu_keys.py --socket /tmp/xemu.sock --text m

The table below is the MEGA65 keyboard matrix, not ASCII or PETSCII: 'm' is
$24 because of where it sits in the matrix, not because of its character code.
"""

import argparse
import socket
import sys
import time

# Matrix position per character. Keys needing shift carry it in the second slot.
SHIFT = 0x0F
NO_KEY = 0x7F

MATRIX = {
    "\r": 0x01, "\n": 0x01,
    "3": 0x08, "w": 0x09, "a": 0x0A, "4": 0x0B, "z": 0x0C, "s": 0x0D, "e": 0x0E,
    "5": 0x10, "r": 0x11, "d": 0x12, "6": 0x13, "c": 0x14, "f": 0x15, "t": 0x16,
    "x": 0x17, "7": 0x18, "y": 0x19, "g": 0x1A, "8": 0x1B, "b": 0x1C, "h": 0x1D,
    "u": 0x1E, "v": 0x1F, "9": 0x20, "i": 0x21, "j": 0x22, "0": 0x23, "m": 0x24,
    "k": 0x25, "o": 0x26, "n": 0x27, "+": 0x28, "p": 0x29, "l": 0x2A, "-": 0x2B,
    ".": 0x2C, ":": 0x2D, "@": 0x2E, ",": 0x2F, "}": 0x30, "*": 0x31, ";": 0x32,
    "=": 0x35, "/": 0x37, "1": 0x38, "_": 0x39, "2": 0x3B, " ": 0x3C, "q": 0x3E,
}

# Characters reached by holding shift over another key.
SHIFTED = {"!": "1", '"': "2", "#": "3", "$": "4", "%": "5",
           "(": "8", ")": "9", "?": "/", "<": ",", ">": "."}

# Named keys, for things a character cannot express.
#
# HELP and F9-F14 are not here, and not because they are out of range: the
# matrix is 72 keys, and mega65-core's matrix_to_ascii.vhdl gives position 67
# as HELP ($1F).  Xemu's table agrees, and its virtkey() repacks row*8+col into
# row*16+col, which is how 67 becomes the $83 that appears in its own headers.
# But sending 67 to a freeze menu loads AUDIOMIX, as though it were `A`, and
# sending 10 for `a` does nothing -- so something between the register and the
# key queue is not what these tables describe, and it is not understood.
# At the keyboard Xemu has HELP on Page Up, and CLR on Home.
NAMED = {"return": 0x01, "f1": 0x04, "f3": 0x05, "f5": 0x06, "f7": 0x03,
         "down": 0x07, "right": 0x02, "delete": 0x00, "stop": 0x3F}


# 20ms, the interval matrix65 uses to drive real hardware.  Longer is not
# safer: the keys are released once at the end of a run rather than between
# presses, so an over-long hold just delays the release the scanner waits for.
DELAY = 0.02


def hold_key(sock, code, modifier=NO_KEY):
    """Put a key down.  Nothing is released until release_keys()."""
    sock.sendall(f"sffd3615 {code:02x} {modifier:02x}\n".encode())
    time.sleep(DELAY)


def release_keys(sock):
    sock.sendall(f"sffd3615 {NO_KEY:02x} {NO_KEY:02x} {NO_KEY:02x}\n".encode())
    time.sleep(DELAY)


def press(sock, code, modifier=NO_KEY):
    hold_key(sock, code, modifier)
    release_keys(sock)


def type_text(sock, text):
    for char in text:
        modifier = NO_KEY
        if char in SHIFTED:
            char, modifier = SHIFTED[char], SHIFT
        lowered = char.lower()
        if lowered.isalpha() and char.isupper():
            modifier = SHIFT
        if lowered not in MATRIX:
            sys.exit(f"no matrix code for {char!r}")
        # Released between every key, not just at the end of the string: two of
        # the same character in a row are otherwise one continuous press, and
        # the second is dropped.  "d2000" arrives as "D20".
        press(sock, MATRIX[lowered], modifier)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="Xemu's -uartmon path")
    parser.add_argument("--text", default="", help="characters to type")
    parser.add_argument("--key", action="append", default=[],
                        help=f"a named key, one of: {', '.join(sorted(NAMED))}")
    parser.add_argument("--delay", type=float, default=0.0,
                        help="seconds to wait before typing, to let the program draw")
    args = parser.parse_args()

    time.sleep(args.delay)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect(args.socket)
    time.sleep(0.3)

    if args.text:
        type_text(sock, args.text)
    for name in args.key:
        if name not in NAMED:
            sys.exit(f"unknown key {name!r}")
        press(sock, NAMED[name])
    return 0


if __name__ == "__main__":
    sys.exit(main())
