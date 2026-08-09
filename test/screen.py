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
COLUMNS = 40
ROWS = 25
CELL_BYTES = 2


def codes(dump: bytes, row: int) -> list[int]:
    """The screen codes of one row."""
    base = SCREEN_AT + row * ROW_BYTES
    return [dump[base + column * CELL_BYTES] for column in range(COLUMNS)]


def colours(dump: bytes, row: int) -> list[int]:
    """The colour of each cell in one row."""
    base = COLOUR_AT + row * ROW_BYTES
    return [dump[base + column * CELL_BYTES + (CELL_BYTES - 1)] for column in range(COLUMNS)]


def _glyph(code: int) -> str:
    if 0x01 <= code <= 0x1A:
        return chr(code - 1 + ord("A"))
    if 0x20 <= code <= 0x3F:
        return chr(code)
    if code == 0x00:
        return "@"
    return f"{{{code:02X}}}"


def text(dump: bytes, row: int) -> str:
    """One row as something a test can match a phrase against."""
    return "".join(_glyph(code) for code in codes(dump, row))


def screen(dump: bytes) -> str:
    """The whole screen, one row per line."""
    return "\n".join(text(dump, row) for row in range(ROWS))


def load(path: str) -> bytes:
    """A memory dump, checked to be long enough to hold the screen."""
    with open(path, "rb") as handle:
        dump = handle.read()
    needed = SCREEN_AT + ROWS * ROW_BYTES
    if len(dump) < needed:
        raise SystemExit(f"memory dump is {len(dump)} bytes, need at least {needed}")
    return dump
