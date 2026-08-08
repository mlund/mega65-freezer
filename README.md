# MEGA65 freezer

Freeze menu and tools for the MEGA65.

An llvm-mos port of [MEGA65/mega65-freezemenu](https://github.com/MEGA65/mega65-freezemenu),
which builds with cc65; see its README for what the tools are for and how they
are installed. Nothing is shared but the behaviour — the sources, build and
tests here are separate.

`RESTORE` freezes the running program; FREEZER draws the menu and launches the
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

Currently needs a patched llvm-mos: stock clang miscompiles 16-bit loop counters on
45GS02 with `-mlto-zp >= 8`, which hangs MAKEDISK mid-write.

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

The host tests build the portable halves with the host compiler and need
nothing else. The emulator tests need an emulator — found automatically, or
given as `XEMU` — and an SD image, which defaults to Xemu's own. They clone it
and write the build into the clone's filesystem, so the image itself is never
touched. With mtools installed, one further test checks that writer by having
mdir read back what it wrote.

`FREEZER_TRACE` is compile-time, so the serial assertions need their own build:

```
cmake -B build-trace -DFREEZER_TRACE=ON ...
```

## Differences from the cc65 build

### Size

Against the binaries shipped on the MEGA65 SD card, each tool measured at the
point it did the same job as its cc65 counterpart:

| tool       |   cc65 |  llvm |    % |
|------------|-------:|------:|-----:|
| `AUDIOMIX` |  23161 |  9727 |  -58 |
| `MEGAINFO` |  21966 | 10646 |  -52 |
| `MONITOR`  |  18226 |  9353 |  -49 |
| `ROMLOAD`  |  17378 |  8957 |  -48 |
| `MAKEDISK` |  22307 | 11590 |  -48 |
| `SPRITED`  |  31016 | 17128 |  -45 |
| `FREEZER`  |  24700 | 19573 |  -21 |

`MONITOR` is measured before the disassembler, the cc65 monitor's D command
being an empty case. With the disassembler, assembler and bit editor it is
24266.

### Bugs found

Defects present in the cc65 sources:

- `report_unmapped()` called itself. Every unmapped address reached through D,
  F, H, C, T or B recursed until the stack died. Eight call sites. Every
  command had been exercised on mapped memory.
- Two 16-bit overflows, the second of them the same arithmetic in the ROM
  loader.
- The thumbnail frame wrote past the end of the screen.
- Dead code: a ROM-name update flag, a chunk reader, a pre-blank of the drive
  numbers, the touch and joystick handling.

### Testing

`ctest` runs both halves:

- Host tests build the toolchain-independent code — number formatting, block
  moves, the disassembler, the register database, the thumbnail frames — with
  the host compiler and check it against known-good output.
- Emulator tests drive Xemu over its serial monitor, typing at the freezer and
  asserting on the screen dump and, with `FREEZER_TRACE`, on the hypervisor
  serial channel. They clone the SD image rather than writing to it.

### Static analysis

Warnings are errors (`-Wall -Wextra`, and the narrowing half of
`-Wconversion`). clang-tidy runs over every source including
`misc-no-recursion`, whose earlier exclusion is what let `report_unmapped()`
ship. A second, advisory target reports 16-bit overflow and narrowing, `int`
being 16 bits here.

### Dependencies

The generators are Python with no third-party packages: the PNG decoding for
the thumbnail frames is stdlib zlib, so libpng is not needed to build.
MEGAINFO composes its numbers rather than linking printf.

### New features

- `MONITOR`: a disassembler, an assembler, and a bit editor that names and
  describes I/O register bits from mega65-core's iomap.txt.
- Colour schemes, switchable while the tools run.
- The freeze menu's fixed text is built at compile time and drawn as
  positioned fragments.
