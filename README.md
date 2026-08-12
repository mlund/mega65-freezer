# MEGA65 freezer

## Synopsis

A rewrite of [MEGA65/mega65-freezemenu](https://github.com/MEGA65/mega65-freezemenu)
in modern C for the llvm-mos toolchain. The tools do what the cc65 originals do,
in 32-64% fewer bytes ([table below](#size)), which leaves room inside the
34817-byte budget for new work. Most of the C compiles on the host and is tested
there; the rest is driven through the MEGA65 serial link by a test harness, so
far under Xemu, though the same harness would drive hardware. `MONITOR` spends
the freed bytes on a syntax-highlighted 45GS02 (dis)assembler and a bit editor
that names registers and bits from mega65-core's `iomap.txt`. The rewrite also
exposed several bugs in the original, listed below. Every push is built under
strict compiler warnings and clang-tidy.

## Freezer Overview

Pressing `RESTORE` freezes the running program; `FREEZER` draws the menu and launches the
rest. Each tool is a separate program loaded from the SD card:

| tool       | what it does                                |
|------------|---------------------------------------------|
| `FREEZER`  | the menu, slot browser and thumbnail        |
| `MONITOR`  | memory, disassembler, assembler, bit editor |
| `MEGAINFO` | system information                          |
| `AUDIOMIX` | audio mixer                                 |
| `SPRITED`  | sprite editor                               |
| `ROMLOAD`  | ROM chooser                                 |
| `MAKEDISK` | empty disk image                            |

Two kinds of data are read off the card at run time and built here too:
`IOMAP.M65`, the I/O register names and descriptions, and `M65THUMB.M65`,
`C65THUMB.M65` and `C64THUMB.M65`, the frames drawn around a slot's thumbnail.

## Building

Currently needs a patched llvm-mos: stock SDK v23.0.1 miscompiles.
You need to build the llvm-mos-sdk using [llvm-mos @ freezer](https://github.com/mlund/llvm-mos/tree/freezer)
and this step is not detailed here.

```
cmake -DCMAKE_PREFIX_PATH=<prefix> -B build
cmake --build build
```

Each `.M65` must fit 34817 bytes, which the link enforces.

| variable        | what it does                                          |
|-----------------|-------------------------------------------------------|
| `MEGA65_IOMAP`  | path to mega65-core's iomap.txt, to regenerate `IOMAP.M65` |
| `MEGA65_SDIMG`  | path to an SD image, for the emulator tests           |
| `MEGA65_ROM`    | path to MEGA65.ROM, for the disassembler's real-code test |
| `XEMU`          | path to the emulator, if it is not found automatically |
| `FREEZER_TRACE` | tracing on the hypervisor serial channel (OFF)        |
| `LTO_ZP`        | zero-page bytes the LTO allocator may use (210)       |
| `C_MCPU`        | `-mcpu` for our sources, `mos6502` on an unpatched compiler (mos45gs02) |
| `LINK_LIBC`     | link mega65-libc (ON)                                 |

The three paths also read an environment variable of the same name. Set
`CPM_SOURCE_CACHE` to a directory to avoid re-fetching mega65-libc.

They point at files you already have, rather than anything downloaded:

    cmake -B build -DMEGA65_IOMAP=~/github/mega65-core/iomap.txt \
                   -DMEGA65_ROM=~/MEGA65.ROM

`iomap.txt` is generated in [mega65-core](https://github.com/MEGA65/mega65-core)
from the `@IO:` comments in its VHDL; `MEGA65.ROM` is the machine's own ROM
image. Neither is ours to redistribute, so neither is included here:
`IOMAP.M65` is committed instead, and without a ROM the disassembler's
real-code test does not run.

Other targets: `format`, `tidy`, `tidy-arithmetic`, `iomap-names`.

## Testing

```
ctest --test-dir build
```

The host tests build portable parts with the host compiler and need
nothing else. The emulator tests need an emulator — found automatically, or
given as `XEMU` — and an SD image, which defaults to Xemu's own. They clone it
and write the build into the clone's filesystem, so the image itself is never
touched. With `mtools` installed, one further test checks that writer by having
mdir read back what it wrote.

`FREEZER_TRACE` is compile-time, so the serial assertions need their own build:

```
cmake -B build-trace -DFREEZER_TRACE=ON ...
```

## Differences from the cc65 build

### Size

Against the binaries shipped on the MEGA65 SD card:

| tool       |   cc65 |  llvm |    % |
|------------|-------:|------:|-----:|
| `AUDIOMIX` |  23161 |  8268 |  -64 |
| `MAKEDISK` |  22307 |  9981 |  -55 |
| `MEGAINFO` |  21966 | 10356 |  -53 |
| `ROMLOAD`  |  17378 |  8741 |  -50 |
| `MONITOR`  |  18226 |  9353 |  -49 |
| `SPRITED`  |  31016 | 15292 |  -51 |
| `FREEZER`  |  24700 | 16822 |  -32 |

`MONITOR` alone is measured _before_ feature additions, at the point it did the
same job as its cc65 counterpart: its disassembler, assembler and bit editor
have nothing to compare against.

### New features

- `MONITOR`:
  - 45GS02 (dis)assembler
  - bit editor with named I/O register bits from mega65-core's `iomap.txt`.
- Dynamic colour schemes
- `MAKEDISK`: the border reports while the card is busy. Formatting is
  thousands of sector operations behind a screen that says only "CREATING
  IMAGE...", and an emulated card answers instantly where a real one does not,
  so the wait had nothing to distinguish it from a hang.
- SD traffic can be counted: `-DSDCARD_COUNTERS=ON` builds three counters that
  a test reads by name, `test/verify_sdcount_xemu.py` reporting what creating a
  D81 costs. Transactions rather than time -- the emulator has no card -- so
  the figures are the half that carries to hardware.
- The freeze menu's fixed text is built at compile time and drawn as
  positioned fragments.

### Bugs found (and fixed)

Defects found in original:

- The freeze slot is SD sectors, and `freeze_poke()` reads a whole 512-byte
  sector, changes one byte and writes it back. Storing the mounted image's name
  and flags a byte at a time therefore cost 68 sector operations for 34 bytes,
  twice on every boot: 136 where 12 do. `freeze_common.c:291-294`.
- Each branch of the PAL/NTSC toggle wrote the sprite Y adjust twice with the
  same value, on both the frozen side and the freezer's own — four more sector
  transfers per keypress, for bytes already there. `freezer.c:1118/1125`.
- The chargen fix read the ROM back off the card. Hyppo loads `MEGA65.ROM` to
  `$20000` immediately before it loads the freezer (`hyppo/task.asm`,
  `attempt_loadc65rom`), so the charset was already in RAM; the freezer fetched
  all 131072 bytes again to use the 4096 at offset `$D000`. Every startup pays
  it, between the menu text and the thumbnail, and the shipped card carries
  neither of the smaller files that would have avoided it. `freezer.c:826-829`.
- The disk chooser read the mounted state it saves before previewing an image
  from the wrong place entirely. `0x0400 + drive_id ? 0x35 : 0x15` parses as
  `(0x0400 + drive_id) ? 0x35 : 0x15`, which is the constant `$35`, so the name
  came out of zero page -- the compiler's own variables -- rather than the
  descriptor at `$0435`. Backing out of the chooser then restored whatever that
  happened to hold. Two neighbouring reads had the same defect.
  `freeze_diskchooser.c:594-596`.
- Loading a tall charset wrote to the wrong register. `$D054.5` is PALEMU and
  `$D07A.4` is CHARY16, but both writes went to `$D07A`: PALEMU was never
  touched, and `$D07A.5` -- NOBUGCOMPAT, deprecated in the core -- was set
  instead and never cleared again, so it survived into the resumed program.
  `freeze_romload.c:402-405`.
- Rewriting a sector that would not verify never gave up. The retry loop is
  written `while (tries < 10)` but nothing increments `tries`, so a card that
  kept failing the read-back was rewritten for ever. The read path next to it
  counts its attempts correctly. `fdisk_hal_mega65.c:236`.
- A partial sector store checked that the run fitted in a sector but not that
  it *ended* in one, so a run starting late and running long wrote past the
  512-byte buffer. The sprite editor is the caller that could: it passes a
  runtime address. `frozen_memory.c:237`.
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


