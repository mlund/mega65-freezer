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

__stack = 0xd000;

OUTPUT_FORMAT {
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
