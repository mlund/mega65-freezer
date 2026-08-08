"""Write files into a FAT32 partition inside a disk image.

Enough of FAT32 to replace a file in the root directory, which is what staging a
build onto a card image needs. Everything is partition-relative: the filesystem
starts at a byte offset inside the image, not at zero.

The FAT is held in memory and written back once. Read from the image it would be
one seek and a four-byte read per cluster examined, and a card has half a million
of them, so allocation alone would be hundreds of thousands of system calls.

An overwrite keeps the file's existing clusters where it can -- the common case
here is replacing a file with one of nearly the same size -- so a shorter file
frees only its tail and a longer one appends. Freeing everything first would
also make replacing a large file on a full card fail, having released nothing it
could not immediately reclaim.
"""

from __future__ import annotations

import datetime
import itertools
import struct

# A chain ends at any value from here up; the low 28 bits are what count.
EOC = 0x0FFFFFF8
FREE = 0
MASK = 0x0FFFFFFF

# Directory entries: 32 bytes each, first byte says whether the slot is in use.
ENTRY = 32
END_OF_DIR = 0x00
DELETED = 0xE5
ATTR_LONG_NAME = 0x0F
ATTR_ARCHIVE = 0x20


def _u16(b: bytes, at: int) -> int:
    return struct.unpack_from("<H", b, at)[0]


def _u32(b: bytes, at: int) -> int:
    return struct.unpack_from("<I", b, at)[0]


def packed_time(when: datetime.datetime) -> tuple[int, int]:
    """A moment as the (date, time) pair a directory entry holds.

    Seconds are stored halved, so odd ones round down; the epoch is 1980, which
    is what bounds the year.
    """
    if not 1980 <= when.year <= 2107:
        raise ValueError(f"{when.year} is outside what a directory entry can hold")
    date = ((when.year - 1980) << 9) | (when.month << 5) | when.day
    time = (when.hour << 11) | (when.minute << 5) | (when.second // 2)
    return date, time


def short_name(name: str) -> bytes:
    """`FREEZER.M65` as the 11 padded bytes a directory entry holds."""
    stem, _, suffix = name.upper().partition(".")
    if len(stem) > 8 or len(suffix) > 3:
        raise ValueError(f"{name} is not an 8.3 name")
    return stem.ljust(8).encode("ascii") + suffix.ljust(3).encode("ascii")


class FAT32:
    def __init__(self, handle, base: int = 0):
        self.f = handle
        self.base = base

        handle.seek(base)
        boot = handle.read(512)

        self.bps = _u16(boot, 11)
        self.spc = boot[13]
        self.reserved = _u16(boot, 14)
        self.nfats = boot[16]
        self.fatsz = _u32(boot, 36)
        self.root = _u32(boot, 44)

        if not self.fatsz or _u16(boot, 17) or _u16(boot, 22):
            raise ValueError("not FAT32: the 16-bit fields should be zero")

        self.cbytes = self.bps * self.spc
        self.data_off = base + (self.reserved + self.nfats * self.fatsz) * self.bps
        self.clusters = (_u32(boot, 32) - self.reserved - self.nfats * self.fatsz) // self.spc

        handle.seek(base + self.reserved * self.bps)
        self.fat = bytearray(handle.read(self.fatsz * self.bps))

    # --- the allocation table, in memory until flush() ---

    def get(self, cluster: int) -> int:
        return _u32(self.fat, cluster * 4) & MASK

    def set(self, cluster: int, value: int) -> None:
        at = cluster * 4
        # The top four bits are reserved and belong to whoever set them.
        keep = _u32(self.fat, at) & ~MASK
        struct.pack_into("<I", self.fat, at, keep | (value & MASK))

    def flush(self) -> None:
        """Write the table back over every copy, and drop the stale free counts."""
        for copy in range(self.nfats):
            self.f.seek(self.base + (self.reserved + copy * self.fatsz) * self.bps)
            self.f.write(self.fat)
        # FSInfo's free-cluster count and next-free hint are now wrong. Both
        # have an "unknown" value, which is honest and costs a reader one scan.
        self.f.seek(self.base + self.bps + 488)
        self.f.write(struct.pack("<II", 0xFFFFFFFF, 0xFFFFFFFF))

    def offset(self, cluster: int) -> int:
        return self.data_off + (cluster - 2) * self.cbytes

    def chain(self, first: int) -> list[int]:
        out: list[int] = []
        cluster = first
        while 2 <= cluster < EOC:
            out.append(cluster)
            cluster = self.get(cluster)
        return out

    def free(self, clusters: list[int]) -> None:
        for cluster in clusters:
            self.set(cluster, FREE)

    def alloc(self, count: int, after: int | None = None) -> list[int]:
        """`count` free clusters, linked, optionally continuing an existing chain."""
        out: list[int] = []
        for cluster in range(2, self.clusters + 2):
            if self.get(cluster) == FREE:
                out.append(cluster)
                if len(out) == count:
                    break
        if len(out) != count:
            raise RuntimeError(f"card full: wanted {count} clusters, found {len(out)}")

        for this, following in itertools.pairwise(out):
            self.set(this, following)
        self.set(out[-1], EOC | 7)
        if after is not None:
            self.set(after, out[0])
        return out

    # --- directories ---

    def entries(self, first: int):
        """Every directory slot as (image offset, 32 bytes), including the free ones."""
        for cluster in self.chain(first):
            base = self.offset(cluster)
            self.f.seek(base)
            block = self.f.read(self.cbytes)
            for at in range(0, len(block), ENTRY):
                yield base + at, block[at : at + ENTRY]

    def find(self, name: bytes) -> tuple[int | None, int | None]:
        """Where `name` lives, and the first slot free for reuse."""
        spare = None
        for at, entry in self.entries(self.root):
            if entry[0] == END_OF_DIR:
                return None, spare if spare is not None else at
            if entry[0] == DELETED:
                if spare is None:
                    spare = at
                continue
            if entry[11] == ATTR_LONG_NAME:
                continue
            if entry[:11] == name:
                return at, spare
        return None, spare

    def grow(self, first: int) -> int:
        """Add a cluster to a directory and return its first slot's offset."""
        last = self.chain(first)[-1]
        cluster = self.alloc(1, after=last)[0]
        self.f.seek(self.offset(cluster))
        self.f.write(bytes(self.cbytes))
        return self.offset(cluster)

    # --- what callers actually want ---

    def write(self, name: str, data: bytes, when: datetime.datetime | None = None) -> None:
        """Create or replace a file in the root directory."""
        eleven = short_name(name)
        at, spare = self.find(eleven)
        fresh = at is None

        if fresh:
            at = spare if spare is not None else self.grow(self.root)
            entry = bytearray(ENTRY)
        else:
            self.f.seek(at)
            entry = bytearray(self.f.read(ENTRY))

        first = (_u16(entry, 20) << 16) | _u16(entry, 26)
        have = self.chain(first) if first >= 2 else []
        want = (len(data) + self.cbytes - 1) // self.cbytes

        if want > len(have):
            more = self.alloc(want - len(have), after=have[-1] if have else None)
            have = have + more
        elif want < len(have):
            self.free(have[want:])
            if want:
                self.set(have[want - 1], EOC | 7)
            have = have[:want]

        for cluster, start in zip(have, range(0, len(data), self.cbytes), strict=True):
            chunk = data[start : start + self.cbytes]
            self.f.seek(self.offset(cluster))
            self.f.write(chunk + bytes(self.cbytes - len(chunk)))

        entry[:11] = eleven
        entry[11] = ATTR_ARCHIVE
        date, time = packed_time(when or datetime.datetime.now())
        # Created only when the entry is, so replacing a file keeps the date it
        # first appeared; written and accessed move every time.
        if fresh:
            struct.pack_into("<HH", entry, 14, time, date)
        struct.pack_into("<H", entry, 18, date)
        struct.pack_into("<HH", entry, 22, time, date)
        head = have[0] if have else 0
        struct.pack_into("<H", entry, 20, head >> 16)
        struct.pack_into("<H", entry, 26, head & 0xFFFF)
        struct.pack_into("<I", entry, 28, len(data))
        self.f.seek(at)
        self.f.write(entry)
