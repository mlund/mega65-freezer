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

/* Low memory the seven tools share.
 *
 * Only what must outlive the program holding it: mega65_dos_exechelper() loads
 * the next tool over this one, so a value handed across that boundary cannot
 * live in either image.  The tools load at $07FF and run upwards, so these sit
 * below that.  Anything a tool merely needs at a particular alignment is an
 * ordinary aligned object and the compiler places it.  Hyppo's
 * freeze_mem_list saves all 384KB of RAM from $0000000 to the freeze slot
 * before the freezer starts and restores it on resume (mega65-core,
 * src/hyppo/freeze.asm), so the frozen program's own bytes here are safe from
 * us and ours are discarded when it resumes.
 *
 * What is not automatic is the tools agreeing with each other: each links
 * separately and sees only its own reservation, so an overlap between two of
 * them is invisible to both.  The whole map therefore lives here, in the one
 * script all seven share, and the assertions below fail the build if any two
 * reservations meet.
 */
tool_density      = 0x033C;   /* disk chooser -> MAKEDISK: 0 = DD, 1 = HD  */
tool_density_size = 0x0001;
tool_drive_id      = 0x033D;  /* disk chooser -> MAKEDISK: which drive     */
tool_drive_id_size = 0x0001;
scheme_handoff      = 0x033E; /* 2 bytes, magic then scheme index          */
scheme_handoff_size = 0x0002;
/* The code that performs the load, which cannot live in the image being
 * overwritten. */
loader_stub      = 0x0340;
loader_stub_size = 0x0040;

ASSERT(tool_density + tool_density_size <= tool_drive_id,
    "low memory: the density byte runs into the drive id")
ASSERT(tool_drive_id + tool_drive_id_size <= scheme_handoff,
    "low memory: the drive id runs into the scheme handoff")
ASSERT(scheme_handoff + scheme_handoff_size <= loader_stub,
    "low memory: the scheme handoff runs into the loader stub")
ASSERT(loader_stub + loader_stub_size <= 0x0400,
    "low memory: the loader stub runs past $03FF")
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
