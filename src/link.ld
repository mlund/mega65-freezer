/* Freezemenu .M65 linker script: C64-style PRG loaded at $0801, entered at
 * $080d via the BASIC SYS stub, and required to end before $9000 (the
 * screen). */

/* The SDK defaults this window to $0090, which is where the C64's BASIC zero
 * page ends and the C64 KERNAL's begins.  The MEGA65 has no such split: its
 * KERNAL reserves all of base page $0000 (0.0002-0.15FF per the user guide),
 * and this program already runs inside that region with interrupts masked,
 * reaching storage through hypervisor traps and DMA rather than KERNAL calls.
 * Widening it gives the register allocator room it measurably wants.
 * $FB-$FE stays free as the conventional user scratch area. */
__basic_zp_start = 0x0002;
__basic_zp_end = 0x00FB;

MEMORY {
    ram (rw) : ORIGIN = 0x0801, LENGTH = 0x87ff
}

INCLUDE commodore.ld

INPUT(unmap-basic.o)

/* format_util_version() scans only the first 512 bytes of a loaded .M65 for
 * the "V:20" stamp, so the string must be pinned this early. */
SECTIONS {
    .entrytramp : { KEEP(*(.entrytramp)) }
    .version : { KEEP(*(.version)) }
} INSERT AFTER .basic_header

/* The loader jumps here blind, so a moved .entrytramp means it executes
 * whatever data took its place.  Must match PROGRAM_ENTRY in helper.h. */
ASSERT(ADDR(.entrytramp) == 0x080d,
    "entry trampoline moved: loader entry is fixed, see PROGRAM_ENTRY in helper.h")

INCLUDE lowmem.ld

ASSERT(tool_density + tool_density_size <= tool_drive_id,
    "low memory: the density byte runs into the drive id")
ASSERT(tool_drive_id + tool_drive_id_size <= scheme_handoff,
    "low memory: the drive id runs into the scheme handoff")
ASSERT(scheme_handoff + scheme_handoff_size <= loader_stub,
    "low memory: the scheme handoff runs into the loader stub")
ASSERT(loader_stub + loader_stub_size <= hyppo_page,
    "low memory: the loader stub runs into hyppo's page")
ASSERT(hyppo_page + hyppo_page_size <= ORIGIN(ram) - 2,
    "low memory: hyppo's page runs into the loaded image")
ASSERT(hyppo_exec_page + hyppo_exec_page_size <= tool_density,
    "low memory: the exec name page runs into the tool handoff bytes")
/* The trap's own rule, so that moving either page fails the build rather than
 * returning dos_errorcode_invalid_address at run time. */
ASSERT((hyppo_page & 0xFF) == 0 && hyppo_page < 0x7F00,
    "hyppo copies only into a page-aligned buffer below $7F00")
ASSERT((hyppo_exec_page & 0xFF) == 0 && hyppo_exec_page < 0x7F00,
    "hyppo copies only into a page-aligned buffer below $7F00")
/* helper.S copies the routine into the reservation, so the routine must fit. */
ASSERT(loadfile_routine_end - loadfile_routine <= loader_stub_size,
    "the loader stub routine outgrew its reservation in low memory")

/* Above the region the linker allocates from, which stops at $9000 so that a
 * tool cannot grow into the character ROM shadow.  The shadow is what the VIC
 * reads there -- never the RAM beneath it -- and $A000-$B7FF is RAM too under
 * the $01 = $36 every tool sets, so this whole span is free for a buffer the
 * VIC is not going to look at.  Being under 64KB, it stays reachable with an
 * ordinary pointer; the thumbnail is picked over a byte at a time, so putting
 * it above the 64KB line would cost a DMA per access.
 *
 * Not in .bss, because .bss shares the $0801-$9000 region with the code: 4KB
 * there is 4KB the C cannot use, even though it never reaches the .M65 file. */
thumbnail_buffer      = 0x9000;
thumbnail_buffer_size = 0x1000;

ASSERT(thumbnail_buffer + thumbnail_buffer_size <= 0xB800,
    "the thumbnail buffer runs into the screen at $B800")

__stack = 0xd000;

OUTPUT_FORMAT {
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
