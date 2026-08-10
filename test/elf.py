"""Find a symbol's address in a linked image.

A scenario that reads a counter out of a running tool has to know where the
linker put it.  Naming the symbol and looking it up keeps the test correct
across builds; an address written into the test would be right until the next
one.  Parsed here rather than shelled out to llvm-nm so the tests need nothing
from the SDK that the emulator does not already require.
"""

import struct

_SHT_SYMTAB = 2
_SYM_SIZE = 16


def symbol(path: str, name: str) -> int:
    """The address `name` is linked at, from `path`'s symbol table."""
    with open(path, "rb") as handle:
        image = handle.read()

    if image[:4] != b"\x7fELF" or image[4] != 1:
        raise ValueError(f"{path} is not a 32-bit ELF")

    (sections_at,) = struct.unpack_from("<I", image, 32)
    entry_size, count = struct.unpack_from("<HH", image, 46)

    for index in range(count):
        header = sections_at + index * entry_size
        (kind,) = struct.unpack_from("<I", image, header + 4)
        if kind != _SHT_SYMTAB:
            continue

        table_at, table_size, strings_index = struct.unpack_from("<III", image, header + 16)
        (strings_at,) = struct.unpack_from(
            "<I", image, sections_at + strings_index * entry_size + 16
        )

        for entry in range(table_at, table_at + table_size, _SYM_SIZE):
            name_at, value = struct.unpack_from("<II", image, entry)
            if not name_at:
                continue
            start = strings_at + name_at
            if image[start : image.index(b"\0", start)].decode() == name:
                return value

    raise KeyError(f"{name} is not in {path}")
