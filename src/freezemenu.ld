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
    .version : { KEEP(*(.version)) }
} INSERT AFTER .basic_header

__stack = 0xd000;

OUTPUT_FORMAT {
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
