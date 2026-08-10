"""Read the MEGA65 screen, and its colours, out of a memory dump.

Xemu's `-dumpscreen` walks one byte per cell.  That is right for the 8-bit text
mode, and wrong for the 16-bit mode these tools use, where a cell is two bytes:
it dumps width * height = 1000 bytes and so reaches only the first 12.5 of the
25 rows.  Everything below is silently absent rather than blank, which reads the
same way in a test and is not.

`-dumpmem` writes all 384KB of main memory instead, so the screen can be read
from $B800 at the correct stride -- and the colours with it, since the first 2KB
of colour memory is mirrored at $1F800.  A colour cell keeps its colour in the
high byte, matching SCREEN_CELL_BYTES - 1 in src/screen.h.

Screen codes, not ASCII: the screen renders with the C64 ROM charset, so A-Z are
$01-$1A.  text() renders what a test wants to match on and marks anything else,
including the graphics range the rules are drawn from, as `{XX}`; codes() gives
the raw bytes for a check that cares about an exact glyph.
"""

SCREEN_AT = 0xB800
COLOUR_AT = 0x1F800
ROW_BYTES = 80
ROWS = 25

# A cell is two bytes in 16-bit character mode and one in 8-bit, so a row is
# either 40 wide or 80.  $D054.0 (CHR16) says which, and the tools differ: the
# freezer and its menus are 16-bit, the monitor is 8-bit.  Ask the machine
# rather than configuring it per test -- one scenario can cross between them.
CHR16 = 0xFFD3054
COLUMNS = 40


def codes(memory: bytes, row: int, origin: int = 0, cell_bytes: int = 2) -> list[int]:
    """The screen codes of one row.

    `origin` is the address `memory` starts at: 0 for a whole-memory dump, or
    SCREEN_AT for just the screen, as read from a running machine.
    """
    base = SCREEN_AT - origin + row * ROW_BYTES
    return [memory[base + column * cell_bytes] for column in range(ROW_BYTES // cell_bytes)]


def colours(memory: bytes, row: int, origin: int = 0, cell_bytes: int = 2) -> list[int]:
    """The colour of each cell in one row."""
    base = COLOUR_AT - origin + row * ROW_BYTES
    return [
        memory[base + column * cell_bytes + (cell_bytes - 1)]
        for column in range(ROW_BYTES // cell_bytes)
    ]


def _glyph(code: int) -> str:
    if 0x01 <= code <= 0x1A:
        return chr(code - 1 + ord("A"))
    if 0x20 <= code <= 0x3F:
        return chr(code)
    if code == 0x00:
        return "@"
    return f"{{{code:02X}}}"


def text(memory: bytes, row: int, origin: int = 0, cell_bytes: int = 2) -> str:
    """One row as something a test can match a phrase against."""
    return "".join(_glyph(code) for code in codes(memory, row, origin, cell_bytes))


def screen(memory: bytes, origin: int = 0, cell_bytes: int = 2) -> str:
    """The whole screen, one row per line."""
    return "\n".join(text(memory, row, origin, cell_bytes) for row in range(ROWS))


SCREEN_BYTES = ROWS * ROW_BYTES


def load(path: str) -> bytes:
    """A memory dump, checked to be long enough to hold the screen."""
    with open(path, "rb") as handle:
        dump = handle.read()
    needed = SCREEN_AT + ROWS * ROW_BYTES
    if len(dump) < needed:
        raise SystemExit(f"memory dump is {len(dump)} bytes, need at least {needed}")
    return dump
