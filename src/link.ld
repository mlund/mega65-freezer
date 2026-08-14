/* The tools' linker script: a C64-style PRG loaded at $0801, entered at $080d
 * via the BASIC SYS stub, and required to end before $9000 (the screen).
 *
 * Three scripts, by who reads them: this one is the tools'.  lowmem.ld is the
 * part the on-target tests must agree with, so they link it too.
 * test_freezeslot.ld is one test's own placement.  Each assertion lives beside
 * what it guards, so the one below stays here: it is the only one that needs
 * the region defined in this file. */

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
    /* $9000-$B7FF: the character ROM shadow is what the VIC reads at $9000, so
     * the RAM beneath it is free, and $A000-$B7FF is RAM under the $01 = $36
     * every tool sets.  Buffers here cost addresses, not budget. */
    highram (rw) : ORIGIN = 0x9000, LENGTH = 0x2800
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

ASSERT(hyppo_page + hyppo_page_size <= ORIGIN(ram) - 2,
    "low memory: hyppo's page runs into the loaded image")

/* NOLOAD keeps these out of the .M65: they occupy addresses, not file bytes.
 * The arrays are defined in C with __attribute__((section)), so each states its
 * size once, where the array is. */
SECTIONS {
    .thumbnail 0x9000 (NOLOAD) : { *(.thumbnail) } > highram
    .sdverify  0xA000 (NOLOAD) : { *(.sdverify) }  > highram
    .netbuf    0xA200 (NOLOAD) : { *(.netbuf) }    > highram
} INSERT AFTER .bss

ASSERT(ADDR(.thumbnail) + SIZEOF(.thumbnail) <= ADDR(.sdverify),
    "high memory: the thumbnail buffer runs into the verify buffer")
ASSERT(ADDR(.sdverify) + SIZEOF(.sdverify) <= ADDR(.netbuf),
    "high memory: the verify buffer runs into the network buffers")
ASSERT(ADDR(.netbuf) + SIZEOF(.netbuf) <= 0xB800,
    "high memory: the buffers run into the screen at $B800")

__stack = 0xd000;

OUTPUT_FORMAT {
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
