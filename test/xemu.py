"""Run a program under Xemu and look at the machine while it runs.

Every emulator test needs the same twenty-element command line, the same socket
dance and the same teardown, and each copy of it is somewhere a fix has to be
repeated -- which is why one test was still reading a truncated screen long
after the others were fixed.  This is that sequence, once.

`read()` is the other half.  `-dumpmem` writes only on exit, so a test that
wants to see two states has been booting the emulator twice; the serial
monitor's `m` command reads any 28-bit address while the machine runs, over the
socket the keyboard is already using.  A boot is about twenty seconds, so the
second one is worth removing.

Use the dump for whole-screen captures and `read()` for the handful of bytes a
check actually needs: `m` returns formatted hex, sixteen bytes per request.
"""

import os
import re
import signal
import socket
import subprocess
import time

# `m <addr>` answers ":AAAAAAAA:" followed by sixteen bytes as hex
# (xemu/targets/mega65/uart_monitor.c, m65mon_dumpmem28).
_LINE = re.compile(rb":([0-9A-Fa-f]{8}):([0-9A-Fa-f]{32})")
_PER_LINE = 16


def connect(path: str, timeout: float = 5.0) -> socket.socket:
    """The serial monitor, once the emulator has had time to open it."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect(path)
    time.sleep(0.3)
    return sock


def read(sock: socket.socket, address: int, length: int) -> bytes:
    """Memory, read from the running machine rather than from a dump."""
    out = bytearray()
    at = address
    while len(out) < length:
        sock.sendall(f"m{at:x}\n".encode())
        deadline = time.time() + 5.0
        seen = b""
        while time.time() < deadline:
            seen += sock.recv(4096)
            found = _LINE.search(seen)
            if found and int(found.group(1), 16) == at:
                out += bytes.fromhex(found.group(2).decode())
                break
        else:
            raise TimeoutError(f"no answer reading ${at:07X}")
        at += _PER_LINE
    return bytes(out[:length])


def launch(
    emulator: str,
    prg: str,
    *,
    sdimg: str | None = None,
    dump: str | None = None,
    socket_path: str,
    drive=None,
    boot: float = 16.0,
    settle: float = 2.0,
    quit_after: float = 20.0,
) -> None:
    """Boot `prg`, hand the monitor socket to `drive`, then shut down.

    `drive(sock)` runs once the machine is up; it may type, and may read()
    memory.  `dump` collects a memory image on exit, for a whole-screen check.

    The socket path must be short: AF_UNIX caps near 104 characters, so it
    cannot live under a long temporary directory.
    """
    argv = [
        emulator,
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
        prg,
    ]
    if sdimg:
        argv += ["-sdimg", sdimg]
    if dump:
        argv += ["-dumpmem", dump]

    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(boot)
        if drive is not None:
            sock = connect(socket_path)
            try:
                drive(sock)
            finally:
                sock.close()
        time.sleep(settle)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=quit_after)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if os.path.exists(socket_path):
            os.unlink(socket_path)
