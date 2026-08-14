#!/usr/bin/env python3
"""Render a FileHost catalogue, as ether65's docs/FILEHOST.md section 2 defines it.

A third account of the format, beside megatalk-tftpd's encoder and the PHP in
the document, and deliberately so: the machine-side decoder is checked against
a writer that shares no code with it, so the two can disagree.

Used to stage a fixture onto a card clone; not part of any build.
"""

import struct

MAGIC = b"M65FHCAT"
VERSION = 1
HEADER = 128
RECORD = 128
TITLE = 40
AUTHOR = 16
PATH = 48

PRG = 0
D81 = 1


def _fixed(text: str, width: int, pad: bytes) -> bytes:
    """Printable ASCII or a question mark, truncated to fit, then padded."""
    clean = "".join(c if 0x20 <= ord(c) <= 0x7E else "?" for c in text)[:width]
    return clean.encode("ascii") + pad * (width - len(clean))


def record(title: str, author: str, path: str, kind: int, size: int) -> bytes:
    out = _fixed(title, TITLE, b" ") + _fixed(author, AUTHOR, b" ") + _fixed(path, PATH, b"\0")
    out += struct.pack("<BI", kind, size)
    return out + bytes(RECORD - len(out))


_KEEPABLE = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
_HASH_DIGITS = "0123456789ABCDEFGHIJKLMNOPQRSTUV"


def catalog_short_name(path: str, kind: int) -> str:
    """The 8.3 name the machine derives for a catalogue path.

    A second account of src/filehost/shortname.c, so a test that stages a file
    under this name and then asks the machine to attach it is checking that the
    two agree rather than checking the machine against itself.
    """
    base = path.rsplit("/", 1)[-1]
    dot = base.rfind(".")
    stem = base[:dot] if dot > 0 else base
    kept = "".join(c for c in stem.upper() if c in _KEEPABLE)[:5] or "FILE"

    # FNV-1a, sixteen bits, over the whole path.
    value = 0x811C
    for byte in path.encode("ascii"):
        value = ((value ^ byte) * 0x0193) & 0xFFFF

    tail = ""
    for _ in range(3):
        tail += _HASH_DIGITS[value & 31]
        value >>= 5

    return f"{kept}{tail}.{'D81' if kind == D81 else 'PRG'}"


def catalogue(records: list[bytes]) -> bytes:
    """The header, then the records, in the order given.

    The document sorts by title; a fixture states the order it wants instead,
    so a test can pin which row a record lands on.
    """
    # Generation time zero: no reader looks at it, and a fixture that changed
    # every run would make a card clone differ for no reason.
    header = struct.pack("<8sBHHI", MAGIC, VERSION, RECORD, len(records), 0)
    return header + bytes(HEADER - len(header)) + b"".join(records)
