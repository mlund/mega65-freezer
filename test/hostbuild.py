"""Compile a host harness, and drive it.

One flag set for every harness built this way, so what a warning means does not
depend on which test found it.

The flags match HOST_CFLAGS in test/CMakeLists.txt, which builds the doctest
tests.  -Wno-char-subscripts because `char` is unsigned on llvm-mos and may be
signed here: a char subscript cannot go negative on the target, so the warning
is about the host rather than about this code.
"""

from __future__ import annotations

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")

FLAGS = ["-std=c2x", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-char-subscripts"]


def build(cc: str, workdir: str, name: str, sources, includes=()) -> str:
    """Compile `sources` into `workdir/name` and hand back the path.

    Paths are taken relative to test/ for a harness and to src/ for anything of
    ours it links; both are given whole by the caller.
    """
    binary = os.path.join(workdir, name)
    command = [cc, *FLAGS]
    for include in (SRC, *includes):
        command += ["-I", include]
    command += ["-o", binary, *sources]
    subprocess.run(command, check=True)
    return binary


def lines(binary: str, script: str, args=()) -> list[str]:
    """Feed a command script in on stdin and split what comes back.

    `args` is whatever the harness wants on its command line before reading --
    a database to open, a mode to run in.
    """
    return subprocess.run(
        [binary, *args], input=script, capture_output=True, text=True, check=True
    ).stdout.splitlines()
