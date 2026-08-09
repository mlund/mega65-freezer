# MEGA65 freezer

Freeze menu and tools for the MEGA65.

An llvm-mos rewrite of [MEGA65/mega65-freezemenu](https://github.com/MEGA65/mega65-freezemenu),
which builds with cc65; see its README for what the tools are for and how they
are installed. Nothing is shared but the behaviour — the sources, build and
tests here are separate.

`RESTORE` freezes the running program; `FREEZER` draws the menu and launches the
rest. Each tool is a separate program loaded from the SD card:

| tool       | what it does                                |
|------------|---------------------------------------------|
| `FREEZER`  | the menu, slot browser and thumbnail        |
| `MONITOR`  | memory, disassembler, assembler, bit editor |
| `MEGAINFO` | system information                          |
| `AUDIOMIX` |  23161 |  8240 |  -64 |
| `MAKEDISK` |  22307 |  9846 |  -56 |
| `MEGAINFO` |  21966 | 10362 |  -53 |
| `ROMLOAD`  |  17378 |  8685 |  -50 |
| `MONITOR`  |  18226 |  9353 |  -49 |
| `SPRITED`  |  31016 | 16790 |  -46 |
| `FREEZER`  |  24700 | 17321 |  -30 |

`MONITOR` alone is measured _before_ feature additions, at the point it did the
same job as its cc65 counterpart: its disassembler, assembler and bit editor
have nothing to compare against.

### Bugs found (and fixed)

Defects found in original:

- The freeze slot is SD sectors, and `freeze_poke()` reads a whole 512-byte
  sector, changes one byte and writes it back. Storing the mounted image's name
  and flags a byte at a time therefore cost 68 sector operations for 34 bytes,
  twice on every boot: 136 where 12 do. `freeze_common.c:291-294`.
- Each branch of the PAL/NTSC toggle wrote the sprite Y adjust twice with the
  same value, on both the frozen side and the freezer's own — four more sector
  transfers per keypress, for bytes already there. `freezer.c:1118/1125`.
- Verifying a sector write DMA'd the card's buffer into a 512-byte array first,
  when the same buffer can be mapped at `$DE00` and compared where it lies. A
  copy per write, and 512 bytes of RAM for the program's whole life.
  `fdisk_hal_mega65.c:225`.
- `report_unmapped()` called itself. Every unmapped address reached through D,
  F, H, C, T or B recursed until the stack died. Eight call sites. Every
  command had been exercised on mapped memory.
- Two 16-bit overflows, the second of them the same arithmetic in the ROM
  loader.
- The thumbnail frame wrote past the end of the screen.
- The disk image writer formatted only half of a D65. It asked the filesystem
  for 85 tracks of 128 sectors and then cleared 85 of 64, so the back half of
  every image kept whatever the card had held there.
- Every file it created was dated a day early. The RTC reports the day of the
  month from one and the FAT directory stores it from one, and the reading
  subtracted one in between. In twelve-hour mode the same reading made noon
  24:00 and midnight 12:00.
- The check for a name already on the disk could never match: the name built
  out of the directory entry was never terminated, and the trim of the padding
  began one character past the end of it.
- The FAT chain was written one sector too long, out of a buffer that had not
  been cleared, so the tail carried the previous sector's bytes.
- Two that need a card with other than 4KB clusters to show: a file's first
  sector was worked out with a fixed eight sectors per cluster, and the
  end-of-chain test read $0F000000 where FAT32 ends a chain at $0FFFFFF8.
- A text entry field cleared its first cell once per character instead of
  clearing the field.
- Dead code: a ROM-name update flag, a chunk reader, a pre-blank of the drive
  numbers, the touch and joystick handling, a copy of the filename into a page
  the attach writes itself.

### Testing

`ctest` runs:

- Host tests build the toolchain-independent code — number formatting, block
  moves, the disassembler, the register database, the thumbnail frames, the
  disk geometry and FAT records — with the host compiler and check it against
  known-good output. The disk numbers are checked against what the machine
  requires, taken from `mega65-core`: the image sizes hyppo measures to tell a
  D65 from a D81, and the RTC fields named in `iomap.txt`.
- Emulator tests drive Xemu over its serial monitor, typing at the freezer and
  asserting on the screen dump and, with `FREEZER_TRACE`, on the hypervisor
  serial channel. They clone the SD image rather than writing to it.

### Static analysis

Warnings are errors (`-Wall -Wextra`, and the narrowing half of
`-Wconversion`). clang-tidy runs over every source including
`misc-no-recursion`. A second, advisory target reports 16-bit overflow and narrowing, `int`
being 16 bits here.

### Dependencies

Generators are Python with no third-party packages like libpng, ophis.
Pinned mega65-libc is automatically sources by CMake.

### New features

- `MONITOR`: added disassembler, assembler, and bit editor that names and
  describes I/O register bits from mega65-core's iomap.txt.
- Colour schemes, switchable while the tools run.
- The freeze menu's fixed text is built at compile time and drawn as
  positioned fragments.
