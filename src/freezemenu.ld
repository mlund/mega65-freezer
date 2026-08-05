/* Freezemenu .M65 linker script: C64-style PRG loaded at $0801, entered at
 * $080d via the BASIC SYS stub, and required to end before $9000 (the
 * screen). */

__basic_zp_start = 0x0002;
__basic_zp_end = 0x0090;

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
