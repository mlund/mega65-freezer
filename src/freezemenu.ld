/* Freezemenu .M65 linker script: C64-style PRG loaded at $0801, entered at
 * $080d via the BASIC SYS stub, and required to end before $9000 (the screen).
 * Mirrors what cc65's -t c64 produced for this project. */

__basic_zp_start = 0x0002;
__basic_zp_end = 0x0090;

MEMORY {
    ram (rw) : ORIGIN = 0x0801, LENGTH = 0x87ff
}

INCLUDE commodore.ld

INPUT(unmap-basic.o)

/* freeze_megainfo.c's format_util_version() scans only the first 512 bytes of
 * a loaded .M65 for the "V:20" stamp.  cc65 got this for free because
 * version.s was first in every Makefile file list; under LTO the string lands
 * wherever, so pin it immediately after the BASIC header. */
SECTIONS {
    .entrytramp : { KEEP(*(.entrytramp)) }
    .version : { KEEP(*(.version)) }
} INSERT AFTER .basic_header

/* The loader jumps to a fixed address, so .entrytramp landing anywhere else
 * means the loader executes whatever data took its place -- silently, and with
 * no crash at the point of the fault.  Anything inserted ahead of .entrytramp,
 * or a change to .basic_header's size, does exactly that.  Keep in step with
 * PROGRAM_ENTRY in mega65.inc, which hard-codes the same address. */
ASSERT(ADDR(.entrytramp) == 0x080d,
    "entry trampoline moved: loader entry is fixed, see PROGRAM_ENTRY in mega65.inc")

__stack = 0xd000;

OUTPUT_FORMAT {
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
