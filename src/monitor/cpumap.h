#pragma once

#include "addr28.h"

#include <stdbool.h>
#include <stdint.h>

/* The frozen CPU's address translation, worked out in software.
 *
 * A 16-bit address names a window, not a location, and the machine that
 * decided which is gone: the freeze slot holds its registers, not its
 * behaviour.  So the four mechanisms gs4510.vhdl's resolve_address_to_long
 * applies have to be applied here instead, in the order it applies them.
 * Nothing below reads hardware or the card, which is what lets
 * test/verify_cpumap.py run it against the VHDL's own logic on the host.
 *
 * The cartridge port is absent on purpose: EXROM and GAME are lines, not
 * registers, so a freeze slot cannot say what they were. */

/* Take the mapping state from `saved`, the eight bytes the freeze slot holds at
 * $D640+$0A, and the frozen $D030.  A function rather than exported variables
 * because the block has a layout: MAPLO and MAPHI are stored high byte first,
 * which nothing else in that save area does (gs4510.vhdl:4926), and keeping
 * that here is what puts it within reach of the host test.
 *
 * $D030 is the one in the I/O page at $FFD3030; hyppo saves $FFD0030 as its own
 * region but that is the C128 2MHz register, not VIC-III banking. */
void cpumap_load(const uint8_t* saved, uint8_t rom_banking);

/* The 28-bit address the frozen CPU would have reached for `cpu_address`.
 * `writing` selects the write path, which is not the read path: a ROM banked
 * in for reading shadows to the RAM beneath it when written, as on a C64, so
 * storing to an address resolves elsewhere than examining it. */
Addr28 resolve_cpu_address(uint16_t cpu_address, bool writing);

/* Bit 31 of a typed address means "as the frozen CPU saw it", the convention the
 * MEGA65 monitor documents (the Book, "Addresses", with the example G 80001500).
 * Unsigned, because the flag is the sign bit of the 32-bit value the user typed
 * -- which is also why a typed address is not an Addr28 until it is resolved. */
constexpr uint32_t CPU_VIEW_FLAG = 0x80000000UL;

/* An address as typed, resolved through the map if it carries CPU_VIEW_FLAG and
 * passed through if not.  Either way the answer is 28 bits, so the flag and the
 * three bits beside it can never reach the freeze slot.
 *
 * The 32-bit argument is marshalled at each call site, which is still cheaper
 * than the alternative: testing the flag at all three instead, to reach
 * resolve_cpu_address() with a free 16-bit argument, measured worse. */
Addr28 cpumap_typed_address(uint32_t typed);

/* Which mechanism settled a region.  Indexes the CPUMAP_NAME, CPUMAP_MASK and
 * CPUMAP_REGISTER tables in cpumap_labels.inc, so this order is the generator's
 * CPUMAP_MECHANISMS order and an entry added here needs one there. */
typedef enum : uint8_t {
    CpuMapRam,
    CpuMapPort,     /* $01 bits 0-2 */
    CpuMapRom8,     /* the four $D030 banking bits */
    CpuMapRomA,
    CpuMapRomC,
    CpuMapRomE,
    CpuMapCram2k,   /* clear, so the expansion area reaches the cartridge */
    CpuMapMapLow,
    CpuMapMapHigh,
} CpuMapMechanism;

/* One run of 16-bit space that resolves the same way throughout. */
typedef struct {
    uint16_t first;
    uint16_t last; /* inclusive, so $FFFF ends the walk and cannot wrap */
    Addr28 target;
    CpuMapMechanism by;
} CpuMapRun;

/* The run starting at `from`, which must be 4KB-aligned: windows are merged
 * while both the deciding mechanism and the destination stay contiguous.  Walk
 * with `from = run.last + 1` until `run.last` is $FFFF. */
void cpumap_run(uint16_t from, CpuMapRun* run);
