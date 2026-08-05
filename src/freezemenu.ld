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
 * whatever data took its place.  Must match PROGRAM_ENTRY in mega65.inc. */
ASSERT(ADDR(.entrytramp) == 0x080d,
    "entry trampoline moved: loader entry is fixed, see PROGRAM_ENTRY in mega65.inc")

__stack = 0xd000;

OUTPUT_FORMAT {
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
