/*
  Memory monitor for the freezer.

  We know where the freeze regions are from freeze_region_list[],
  so we can convert requested addresses into offsets in the freeze slot.

  We can also allow viewing/modification of the raw freeze slot data itself.

  MEGA65 only: hardware registers and the freeze slot.  The decoding and
  block logic it drives lives in disasm.c and blockmove.c, which are not.
*/

#include "bitedit.h"
#include "blockmove.h"
#include "colours.h"
#include "console.h"
#include "cpumap.h"
#include "disasm.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"
#include "trace.h"

#include <mega65.h>
#include <stdio.h>
#include <string.h>

unsigned char char_to_hex(char in) {
    switch (in) {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return in - '0';
        case 'a':
        case 'b':
        case 'c':
        case 'd':
        case 'e':
        case 'f':
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
            return (in - 'A') & 0xf;
        default:
            break;
    }
    return 0;
}

static unsigned char screen_line_buffer[80];
static unsigned char screen_line_length = 0;
static unsigned char screen_line_offset = 0;

static uint32_t hex_value = 0;
uint32_t mon_address = 0;

static char output_buffer[80];

static unsigned char mon_sector[SD_SECTOR_SIZE];
static uint32_t mon_sector_num = 0xffffffffUL;

/* The frozen PC, resolved to 28 bits so a command taking an address can name
 * the location the program reached rather than the window it saw.  Cached by
 * show_registers() alongside the mapping state itself, which lives in
 * cpumap.c; the sentinel keeps an unread PC from matching any real address. */
static uint32_t frozen_program_counter = 0xffffffffUL;

/* Set when mon_sector holds changes the card does not, so that a fill spanning
 * sectors writes each one back before the next displaces it. */
static bool mon_sector_dirty = false;

static void flush_sector(void) {
    if (mon_sector_dirty) {
        lcopy((long)mon_sector, (long)sector_buffer, SD_SECTOR_SIZE);
        sdcard_writesector(freeze_slot_start_sector + mon_sector_num, 0);
        mon_sector_dirty = false;
    }
}

/* Bring the sector holding `freeze_slot_offset` into mon_sector.  SD reads
 * dominate the cost of every command here, so the tag check guards all callers
 * rather than each keeping its own copy of the same test. */
static void cache_sector(uint32_t freeze_slot_offset) {
    if (mon_sector_num != (freeze_slot_offset >> 9)) {
        flush_sector();
        mon_sector_num = (freeze_slot_offset >> 9);
        sdcard_readsector(freeze_slot_start_sector + mon_sector_num);
        lcopy((long)sector_buffer, (long)mon_sector, SD_SECTOR_SIZE);
    }
}

/* Change one byte of frozen memory, leaving the write for flush_sector(): a
 * fill would otherwise write the same sector once per byte. */
bool block_write_byte(uint32_t address, unsigned char value) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(address);
    if (freeze_slot_offset == FREEZE_SLOT_NOT_PRESENT) {
        return false;
    }
    cache_sector(freeze_slot_offset);
    mon_sector[freeze_slot_offset & 0x1ff] = value;
    mon_sector_dirty = true;
    return true;
}

/* Fetch one byte of frozen memory.  The check is per byte because an
 * instruction can begin in one sector and end in the next; show_memory_line()
 * gets away with one check per line only because its runs are 16-byte aligned. */
bool disasm_read_byte(uint32_t address, uint8_t* value) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(address);
    if (freeze_slot_offset == FREEZE_SLOT_NOT_PRESENT) {
        return false;
    }
    cache_sector(freeze_slot_offset);
    *value = mon_sector[freeze_slot_offset & 0x1ff];
    return true;
}

/* format_hex() emits ASCII, but write_line_raw() bypasses the letter shift that
 * write_line_len() applies, so A-F have to be folded to screen codes here. */
static void hex_to_screen_codes(unsigned char count) {
    for (unsigned char i = 0; i < count; i++) {
        if (output_buffer[i] >= 'A' && output_buffer[i] <= 'F') {
            output_buffer[i] &= 0x0f;
        }
    }
}

/* The marker plus seven hex digits every listing line opens with. */
constexpr uint8_t ADDRESS_FIELD_WIDTH = 8;

/* The marker and seven digits every listing starts with, coloured as the
 * disassembly colours its own address column. */
static void colour_address_field(void) {
    recolour_last_line_segment(0, ADDRESS_FIELD_WIDTH, SchemeAddress);
}

void show_memory_line(uint32_t addr) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);

    lfill((long)output_buffer, 0, 80);
    output_buffer[0] = ':';
    format_hex(&output_buffer[1], addr, 7);
    output_buffer[8] = ' ';

    if (freeze_slot_offset == FREEZE_SLOT_NOT_PRESENT) {
        // Memory that isn't saved
        for (unsigned char i = 0; i < 65; i++) {
            output_buffer[9 + i] =
                "<UNMAPPED OR UNFROZEN MEMORY>                                    "[i] & 0x3f;
        }
        output_buffer[9] = '<';
        output_buffer[9 + 28] = '>';
    } else {
        cache_sector(freeze_slot_offset);
        // Two spaces before character rendering of block
        output_buffer[8 + 16 * 3 + 0] = ' ';
        output_buffer[8 + 16 * 3 + 1] = ' ';
        for (unsigned char i = 0; i < 16; i++) {
            unsigned char b = mon_sector[(i + freeze_slot_offset) & 0x1ff];
            // Space before hex
            output_buffer[8 + i * 3] = ' ';
            // hex digits
            format_hex(&output_buffer[8 + 1 + i * 3], b, 2);
            // C64 character rendering of the same byte
            output_buffer[8 + 16 * 3 + 2 + i] = b;
        }
    }
    hex_to_screen_codes(8 + 16 * 3);

    write_line_raw(output_buffer, 0, 8 + 16 * 3 + 2 + 16);
    colour_address_field();
}

void show_memory(void) {
    for (unsigned char i = 0; i < 16; i++) {
        show_memory_line(mon_address);
        mon_address += 16;
    }
}

static void report_error(const char* message) {
    write_line(message, 0);
    recolour_last_line(SchemeError);
}

/* The failure every command that walks frozen memory can reach. */
static void report_unmapped(void) {
    report_error("? UNMAPPED OR UNFROZEN ADDRESS  ERROR");
}

/* A screenful, matching show_memory()'s sixteen lines. */
constexpr uint8_t DISASSEMBLY_LINES = 16;

/* Indexed by DisasmMnemonicClass: plain, control flow, MEGA65-only. */
static const unsigned char MNEMONIC_CLASS_COLOUR[] = {
    SchemeAsmPlain, SchemeAsmControlFlow, SchemeAsmMega65};

/* Colour the fields of the line just written.  `attribute` is OR'd into every
 * field so a whole-line marker such as AttribReverse leaves no gaps; the
 * mnemonic is coloured across its padded field for the same reason. */
static void colour_disassembly_line(const DisassemblyLayout* layout, unsigned char attribute) {
    unsigned char mnemonic_colour = MNEMONIC_CLASS_COLOUR[layout->mnemonic_class];
    unsigned char operand_column = layout->mnemonic_column + DISASM_MNEMONIC_FIELD_WIDTH;

    recolour_last_line_segment(0, DISASM_BYTE_COLUMN, SchemeAddress | attribute);
    recolour_last_line_segment(DISASM_BYTE_COLUMN,
        layout->mnemonic_column - DISASM_BYTE_COLUMN,
        SchemeAsmBytes | attribute);
    recolour_last_line_segment(
        layout->mnemonic_column, DISASM_MNEMONIC_FIELD_WIDTH, mnemonic_colour | attribute);
    if (layout->text_length > operand_column) {
        recolour_last_line_segment(
            operand_column, layout->text_length - operand_column, SchemeAsmOperand | attribute);
    }
}

/* Print the instruction at mon_address and step past it.  False means frozen
 * memory does not reach there, and mon_address is left alone. */
static bool show_disassembly_line(void) {
    char text[DISASM_TEXT_MAX];
    DisassemblyLayout layout;

    if (!disassemble_instruction(mon_address, text, &layout)) {
        return false;
    }
    /* After the read, not before: the bytes have come back out of the freeze
     * slot and been decoded, so this is what is on the card rather than what a
     * caller believed it wrote.  disassemble_instruction NUL-terminates. */
    TRACE(text);
    /* The saved PC is 16 bits, so it names a CPU address whose physical bank
     * depends on MAP and $01 state this code does not decode.  Marking only
     * bank 0 gives up the highlight on a banked PC rather than reverse-videoing
     * an unrelated line -- ROM at $3FA23 and RAM at $0FA23 both match $FA23. */
    unsigned char attribute = (mon_address == frozen_program_counter) ? AttribReverse : 0;

    write_line_len(text, 0, layout.text_length);
    colour_disassembly_line(&layout, attribute);
    mon_address += layout.length;
    return true;
}

void show_disassembly(void) {
    for (unsigned char line = 0; line < DISASSEMBLY_LINES; line++) {
        if (!show_disassembly_line()) {
            report_unmapped();
            /* Step past the hole so a repeated D can walk out of it, as M does.
             * One byte, because without a decode there is no boundary to trust. */
            mon_address++;
            return;
        }
    }
}

void set_memory(void) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(mon_address);
    unsigned char i = 0;

    if (freeze_slot_offset == FREEZE_SLOT_NOT_PRESENT) {
        report_unmapped();
        return;
    } else {
        cache_sector(freeze_slot_offset);

        freeze_slot_offset &= 0x1ff;

        // Now accept various forms of input for setting memory.
        while (screen_line_offset < screen_line_length) {
            VICIV.bordercol = i;
            switch (screen_line_buffer[screen_line_offset]) {
                case ' ':
                    // Skip spaces
                    screen_line_offset++;
                    break;
                case '\"':
                    // double-quoted string means ASCII
                    screen_line_offset++;
                    while (screen_line_offset < screen_line_length) {
                        // Another double quote ends ASCII input
                        if (screen_line_buffer[screen_line_offset] == '\"') {
                            screen_line_offset++;
                            break;
                        }
                        // Take ASCII literal char
                        mon_sector[(freeze_slot_offset + i) & 0x1ff] =
                            screen_line_buffer[screen_line_offset++];
                        i++;
                    }
                    break;
                case '\'':
                    // double-quoted string means screen char codes
                    screen_line_offset++;
                    while (screen_line_offset < screen_line_length) {
                        // Another single quote ends screen char code input
                        if (screen_line_buffer[screen_line_offset] == '\'') {
                            screen_line_offset++;
                            break;
                        }
                        // Change A-Z and a-z to screen char code equivalents
                        if (((screen_line_buffer[screen_line_offset] >= 'A') &&
                                (screen_line_buffer[screen_line_offset] < 'Z')) ||
                            ((screen_line_buffer[screen_line_offset] >= 'a') &&
                                (screen_line_buffer[screen_line_offset] < 'z'))) {
                            mon_sector[(freeze_slot_offset + i) & 0x1ff] =
                                screen_line_buffer[screen_line_offset++] & 0x1f;
                        } else {
                            mon_sector[(freeze_slot_offset + i) & 0x1ff] =
                                screen_line_buffer[screen_line_offset++];
                        }
                        i++;
                    }
                    break;
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                case 'a':
                case 'b':
                case 'c':
                case 'd':
                case 'e':
                case 'f':
                case 'A':
                case 'B':
                case 'C':
                case 'D':
                case 'E':
                case 'F':
                    // hex byte
                    mon_sector[(freeze_slot_offset + i) & 0x1ff] =
                        char_to_hex(screen_line_buffer[screen_line_offset++]);
                    if (screen_line_buffer[screen_line_offset] != ' ') {
                        mon_sector[(freeze_slot_offset + i) & 0x1ff] =
                            (unsigned char)(mon_sector[(freeze_slot_offset + i) & 0x1ff] << 4);
                        mon_sector[(freeze_slot_offset + i) & 0x1ff] |=
                            char_to_hex(screen_line_buffer[screen_line_offset++]);
                    }
                    i++;
                    break;
                default:
                    write_line("? SYNTAX  ERROR", 0);
                    recolour_last_line(SchemeError);
                    return;
            }
        }

        // Write changes back
        lcopy((long)mon_sector, (long)sector_buffer, SD_SECTOR_SIZE);
        sdcard_writesector(freeze_slot_start_sector + mon_sector_num, 0);

        // After writing memory values, redisplay the modified region
        mon_sector_num = -1;
        show_memory();
    }
}

static const char REG_DESC_LINE[] =
    "PC   IRQ  NMI  A  X  Y  Z  B  SP   FLAGS    $01   MAPLO   MAPHI   PC28";
/* Column of each field in REG_DESC_LINE. */
constexpr uint8_t REGLINE_PC = 0;
/* show_registers() never fills these two, so the header advertises IRQ and NMI
 * over blank columns.  Shared with the cc65 monitor. */
[[maybe_unused]] constexpr uint8_t REGLINE_IRQ = 5;
[[maybe_unused]] constexpr uint8_t REGLINE_NMI = 10;
constexpr uint8_t REGLINE_A = 15;
constexpr uint8_t REGLINE_X = 18;
constexpr uint8_t REGLINE_Y = 21;
constexpr uint8_t REGLINE_Z = 24;
constexpr uint8_t REGLINE_B = 27;
constexpr uint8_t REGLINE_SP = 30;
constexpr uint8_t REGLINE_FLAGS = 35;
constexpr uint8_t REGLINE_01 = 44;
constexpr uint8_t REGLINE_MAPLO = 50;
constexpr uint8_t REGLINE_MAPHI = 58;
constexpr uint8_t REGLINE_PC28 = 66;
#include "cpumap_labels.inc"

/* Where the map's four columns start.  The heading is written to the same
 * offsets, so changing one means changing MAP_DESC_LINE with it. */
constexpr uint8_t MAP_TARGET_COLUMN = 13;
constexpr uint8_t MAP_CONTENTS_COLUMN = 30;
constexpr uint8_t MAP_DECIDED_COLUMN = 42;

/* As much of a row as write_line() puts on screen. */
constexpr uint8_t MAP_ROW_WIDTH = 78;

/* Where "%0000.0000" ends, and so where a row's dimming starts. */
constexpr uint8_t MAP_MASK_END = MAP_DECIDED_COLUMN + 10;

/* Columns 0, 13, 30 and 42, matching the constants above. */
static const char MAP_DESC_LINE[] = "CPU RANGE    28-BIT RANGE     CONTENTS    DECIDED BY";

/* Text into the row, returning the column after it. */
static uint8_t put_text(uint8_t at, const char* text) {
    while (*text) {
        output_buffer[at++] = *text++;
    }
    return at;
}

/* A bit mask as %0000.1000: which bit, without the reader having to know that
 * iomap.txt's `.3` counts from the right.  The nibble separator is a full stop
 * because the screen is the C64 ROM charset, which has no underscore. */
static uint8_t put_mask(uint8_t at, uint8_t mask) {
    output_buffer[at++] = '%';
    for (uint8_t bit = 0x80; bit != 0; bit >>= 1) {
        if (bit == 0x08) {
            output_buffer[at++] = '.';
        }
        output_buffer[at++] = (mask & bit) ? '1' : '0';
    }
    return at;
}

/* What the frozen program would find at a 28-bit address.  Banks 2 and 3 hold
 * whatever ROM was loaded, so the names below -- the standard layout -- each
 * carry a question mark instead of the tool identifying the image, which is a
 * sector read and a table of version strings to answer the same way on every
 * machine running the shipped ROM.  MEGAINFO reports the version. */
static const char* map_contents(uint32_t target) {
    /* Compared as 4KB pages: every boundary that matters is page-aligned, and
     * 16-bit comparisons are markedly cheaper here than 28-bit ones. */
    uint16_t page = (uint16_t)(target >> 12);

    if (page == 0xFFD3) {
        return "I/O";
    }
    if (page == 0x7FFD) {
        return "CART I/O";
    }
    if (page >= 0x020 && page < 0x040) {
        if (page >= 0x02E && page < 0x030) {
            return "C64 KERNAL?";
        }
        if (page == 0x02D) {
            return "CHARSET?";
        }
        if (page >= 0x02A && page < 0x02C) {
            return "C64 BASIC?";
        }
        return "C65 ROM?";
    }
    return page < 0x060 ? "RAM" : "";
}

/* The register and bit that settled a run, or the offset for a mapped one.  A
 * mechanism with no name is one where nothing was consulted -- unmapped RAM --
 * and prints nothing at all.  Returns the column the row's dimming should start
 * at, or 0 for a row with nothing worth picking out. */
static bool put_decided_by(const CpuMapRun* run) {
    uint8_t mask = CPUMAP_MASK[run->by];
    uint8_t at = MAP_DECIDED_COLUMN;

    if (mask == 0) {
        if (CPUMAP_NAME[run->by][0] == '\0') {
            return false;
        }
        /* The offset is what the run moved by, which the run already states,
         * so the MAP register itself never has to be read back here.  Only the
         * top three digits are shown: it is a multiple of $100. */
        Addr28 offset = (run->target - run->first) & 0xFFFFFL;

        at = put_text(at, CPUMAP_NAME[run->by]);
        at = put_text(at, " +$");
        format_hex(&output_buffer[at], offset >> 8, 3);
        output_buffer[at + 3] = '0';
        output_buffer[at + 4] = '0';
        /* Nothing to dim: the whole field is the offset. */
        return false;
    }

    at = put_mask(at, mask);
    at = put_text(at, " mask of ");
    at = put_text(at, CPUMAP_REGISTER_NAME[CPUMAP_REGISTER[run->by]]);
    at = put_text(at, " (");
    at = put_text(at, CPUMAP_NAME[run->by]);
    output_buffer[at] = ')';
    return true;
}

/* Where every part of the frozen 16-bit space actually led, one row per run of
 * windows that resolved the same way.  Printed under the registers because the
 * MAPLO/MAPHI/$01 values there are what it explains. */
static void show_memory_map(void) {
    CpuMapRun run;
    uint16_t at = 0;

    write_line(MAP_DESC_LINE, 0);
    recolour_last_line(SchemeTextDim);

    do {
        cpumap_run(at, &run);

        lfill((long)output_buffer, ' ', 80);
        format_hex(&output_buffer[0], run.first, 4);
        output_buffer[4] = '-';
        format_hex(&output_buffer[5], run.last, 4);
        output_buffer[10] = '-';
        output_buffer[11] = '>';
        format_hex(&output_buffer[MAP_TARGET_COLUMN], run.target, 7);
        output_buffer[MAP_TARGET_COLUMN + 7] = '-';
        format_hex(&output_buffer[MAP_TARGET_COLUMN + 8],
            run.target + (run.last - run.first), 7);
        put_text(MAP_CONTENTS_COLUMN, map_contents(run.target));
        bool masked = put_decided_by(&run);
        write_line(output_buffer, 0);

        /* The addresses in the same colour every other address in this monitor
         * gets, and everything past the mask dimmed so the mask stands out. */
        recolour_last_line_segment(0, MAP_CONTENTS_COLUMN, SchemeAddress);
        if (masked) {
            recolour_last_line_segment(MAP_MASK_END, MAP_ROW_WIDTH - MAP_MASK_END, SchemeTextDim);
        }

        at = (uint16_t)(run.last + 1);
    } while (run.last != 0xFFFF);
}

void show_registers(void) {
    // Get hypervisor register backup area
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(0xFFD3640U);
    uint16_t value;

    lfill((long)output_buffer, ' ', 80);

    /* Test the sentinel before shifting: 0xFFFFFFFF >> 9 is 0x7FFFFF, so a
     * shifted value can never equal it and the guard would never fire. */
    if (freeze_slot_offset == FREEZE_SLOT_NOT_PRESENT) {
        write_line("? FROZEN REGISTERS NOT FOUND  ERROR", 0);
        recolour_last_line(SchemeError);
    } else {
        /* Its own sector, into the buffer the register block is read from
         * below, so it comes first and is held across that read. */
        uint8_t rom_banking = freeze_io_peek(0x3030);

        freeze_slot_offset = freeze_slot_offset >> 9L;
        sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);

        /* The heading first, dimmed as the map's heading below it is: both
         * label a block of values rather than being values themselves. */
        write_line(REG_DESC_LINE, 0);
        recolour_last_line(SchemeTextDim);

        // Now prepare the line of actual register values

        // $D640-$D67F is frozen as a single piece, so the offsets are $00, not $40 from the
        // beginning

        /* Before the PC, which is resolved through the map it describes. */
        cpumap_load(&sector_buffer[0x0A], rom_banking);

        // PC
        value = sector_buffer[0x08] + (sector_buffer[0x09] << 8);
        format_hex(&output_buffer[REGLINE_PC], value, 4);

        /* The 28-bit address that PC actually reached, which is what M and D
         * accept -- the 16-bit value above names a window, not a location. */
        frozen_program_counter = resolve_cpu_address(value, false);
        format_hex(&output_buffer[REGLINE_PC28], frozen_program_counter, 7);

        // A
        value = sector_buffer[0x00];
        format_hex(&output_buffer[REGLINE_A], value, 2);

        // X
        value = sector_buffer[0x01];
        format_hex(&output_buffer[REGLINE_X], value, 2);

        // Y
        value = sector_buffer[0x02];
        format_hex(&output_buffer[REGLINE_Y], value, 2);

        // Z
        value = sector_buffer[0x03];
        format_hex(&output_buffer[REGLINE_Z], value, 2);

        // B
        value = sector_buffer[0x04];
        format_hex(&output_buffer[REGLINE_B], value, 2);

        // SP
        value = sector_buffer[0x05] + (sector_buffer[0x06] << 8);
        format_hex(&output_buffer[REGLINE_SP], value, 4);

        // $00/$01 CPU port
        value = sector_buffer[0x10];
        format_hex(&output_buffer[REGLINE_01], value, 2);
        value = sector_buffer[0x11];
        output_buffer[REGLINE_01 + 2] = '/';
        format_hex(&output_buffer[REGLINE_01 + 3], value, 2);

        // FLAGS
        value = sector_buffer[0x07];
        output_buffer[REGLINE_FLAGS + 0] = (value & 0x80) ? 'N' : '-';
        output_buffer[REGLINE_FLAGS + 1] = (value & 0x40) ? 'V' : '-';
        output_buffer[REGLINE_FLAGS + 2] = (value & 0x20) ? 'E' : '-';
        output_buffer[REGLINE_FLAGS + 3] = (value & 0x10) ? 'B' : '-';
        output_buffer[REGLINE_FLAGS + 4] = (value & 0x08) ? 'D' : '-';
        output_buffer[REGLINE_FLAGS + 5] = (value & 0x04) ? 'I' : '-';
        output_buffer[REGLINE_FLAGS + 6] = (value & 0x02) ? 'Z' : '-';
        output_buffer[REGLINE_FLAGS + 7] = (value & 0x01) ? 'C' : '-';

        /* MAPLO and MAPHI read high byte first, so displaying them as little
         * endian words swapped the selection nibble into the offset. */
        // MAPLO
        format_hex(&output_buffer[REGLINE_MAPLO],
            (sector_buffer[0x0A] << 8) | sector_buffer[0x0B], 4);
        value = sector_buffer[0x0E];
        output_buffer[REGLINE_MAPLO + 4] = '/';
        format_hex(&output_buffer[REGLINE_MAPLO + 5], value, 2);

        // MAPHI
        format_hex(&output_buffer[REGLINE_MAPHI],
            (sector_buffer[0x0C] << 8) | sector_buffer[0x0D], 4);
        value = sector_buffer[0x0F];
        output_buffer[REGLINE_MAPHI + 4] = '/';
        format_hex(&output_buffer[REGLINE_MAPHI + 5], value, 2);

        write_line(output_buffer, 0);
        show_memory_map();
    }
}

/* Hex from the typed line, advancing screen_line_offset past it.  Leading
 * spaces are skipped so that "M 1000" and "M1000" read alike. */
unsigned char parse_hex(void) {
    uint8_t digits;

    while (screen_line_buffer[screen_line_offset] == ' ') {
        screen_line_offset++;
    }
    const char* from = (const char*)&screen_line_buffer[screen_line_offset];
    screen_line_offset += (unsigned char)(disasm_parse_hex(from, &hex_value, &digits) - from);
    return digits;
}

/* An address the command cannot do without.  Only parse_range() reaches this,
 * so everything it reads is an address and folds; the values F, H and ; take
 * come straight from parse_hex() and must not. */
static bool parse_required_hex(uint32_t* value) {
    if (!parse_hex()) {
        report_error("? SYNTAX  ERROR");
        return false;
    }
    *value = cpumap_typed_address(hex_value);
    return true;
}

/* The "start end" F, H, C and T open with; end is one past the last address. */
static bool parse_range(uint32_t* start, uint32_t* end) {
    if (!parse_required_hex(start) || !parse_required_hex(end)) {
        return false;
    }
    if (*end <= *start) {
        report_error("? SYNTAX  ERROR");
        return false;
    }
    return true;
}

/* F start end value */
void fill_memory(void) {
    uint32_t start;
    uint32_t end;
    uint32_t address;

    if (!parse_range(&start, &end)) {
        return;
    }
    if (!parse_hex()) {
        report_error("? SYNTAX  ERROR");
        return;
    }

    for (address = start; address < end; address++) {
        if (!block_write_byte(address, (unsigned char)hex_value)) {
            flush_sector();
            report_unmapped();
            return;
        }
    }
    flush_sector();

    /* Show the result rather than announce it, as S does after writing. */
    mon_address = start;
    show_memory();
}

/* Blank output_buffer and open it with the marker and address a listing line
 * starts with. */
static void begin_address_line(char marker, uint32_t address) {
    lfill((long)output_buffer, ' ', 80);
    output_buffer[0] = marker;
    format_hex(&output_buffer[1], address, 7);
}

/* One listed address, as H and C both report them. */
static void write_address_line(uint32_t address) {
    begin_address_line(':', address);
    write_line_len(output_buffer, 0, ADDRESS_FIELD_WIDTH);
    colour_address_field();
}

/* The trailer H and C share: nothing found, or the cap cut the list short. */
static void report_hit_count(
    unsigned char hits, bool truncated, const char* none, const char* more) {
    if (hits == 0) {
        write_line(none, 0);
    } else if (truncated) {
        write_line(more, 0);
        recolour_last_line(SchemeNotice);
    }
}

/* H start end byte [byte...] */
constexpr uint8_t HUNT_PATTERN_MAX = 8;
constexpr uint8_t HUNT_HITS_MAX = 16;

void hunt_memory(void) {
    unsigned char pattern[HUNT_PATTERN_MAX];
    unsigned char pattern_length = 0;
    unsigned char hits = 0;
    uint32_t start;
    uint32_t end;
    uint32_t address;

    if (!parse_range(&start, &end)) {
        return;
    }
    while (pattern_length < HUNT_PATTERN_MAX && parse_hex()) {
        pattern[pattern_length++] = (unsigned char)hex_value;
    }
    if (pattern_length == 0) {
        report_error("? SYNTAX  ERROR");
        return;
    }

    for (address = start; address < end && hits < HUNT_HITS_MAX; address++) {
        unsigned char matched = 0;
        while (matched < pattern_length) {
            unsigned char byte;
            if (!disasm_read_byte(address + matched, &byte) || byte != pattern[matched]) {
                break;
            }
            matched++;
        }
        if (matched == pattern_length) {
            write_address_line(address);
            hits++;
        }
    }

    /* Stopping at the cap keeps a wide range from scrolling its own hits away. */
    report_hit_count(hits, address < end, "NOT FOUND.", "MORE MATCHES FOLLOW.");
}

/* T and C walk two regions at once, and the sector cache holds one sector, so
 * a byte at a time would evict the far end on every byte -- an SD access per
 * byte rather than per chunk.  .bss costs nothing in the image. */
/* C start end other */
void compare_memory(void) {
    unsigned char differences = 0;
    uint32_t start;
    uint32_t end;
    uint32_t other;

    if (!parse_range(&start, &end) || !parse_required_hex(&other)) {
        return;
    }

    uint32_t remaining = end - start;
    while (remaining > 0 && differences < HUNT_HITS_MAX) {
        uint16_t got = block_read(start, remaining);
        if (got == 0) {
            report_unmapped();
            return;
        }
        for (uint16_t i = 0; i < got && differences < HUNT_HITS_MAX; i++) {
            unsigned char there;
            if (!disasm_read_byte(other + i, &there)) {
                report_unmapped();
                return;
            }
            if (block_buffer[i] != there) {
                write_address_line(start + i);
                differences++;
            }
        }
        start += got;
        other += got;
        remaining -= got;
    }

    report_hit_count(differences, remaining > 0, "NO DIFFERENCES.", "MORE DIFFERENCES FOLLOW.");
}

/* T start end destination */
void transfer_memory(void) {
    uint32_t start;
    uint32_t end;
    uint32_t destination;

    if (!parse_range(&start, &end) || !parse_required_hex(&destination)) {
        return;
    }
    bool moved = block_move(start, destination, end - start);
    flush_sector();
    if (!moved) {
        report_unmapped();
        return;
    }
    mon_address = destination;
    show_memory();
}

/* P start -- eight bytes to a character cell, eight cells across. */
constexpr uint8_t BITMAP_CELLS = 8;
constexpr uint8_t BITMAP_ROWS = 2;
constexpr uint8_t BITMAP_COLUMN = 9;
constexpr uint8_t BITMAP_CELL_STRIDE = 9; /* eight pixels and a gap */

/* A screen code, not ASCII: the filled circle keeps a clear column each side,
 * so neighbouring set pixels stay countable where a solid block merges. */
constexpr uint8_t BITMAP_SET = 0x51;

void show_bitmaps(void) {
    for (unsigned char cell_row = 0; cell_row < BITMAP_ROWS; cell_row++) {
        for (unsigned char pixel_row = 0; pixel_row < 8; pixel_row++) {
            if (pixel_row == 0) {
                begin_address_line(',', mon_address);
            } else {
                lfill((long)output_buffer, ' ', 80);
            }
            for (unsigned char cell = 0; cell < BITMAP_CELLS; cell++) {
                unsigned char bits;
                if (!disasm_read_byte(mon_address + (uint32_t)cell * 8 + pixel_row, &bits)) {
                    report_unmapped();
                    return;
                }
                for (unsigned char bit = 0; bit < 8; bit++) {
                    output_buffer[BITMAP_COLUMN + cell * BITMAP_CELL_STRIDE + bit] =
                        (bits & (0x80 >> bit)) ? BITMAP_SET : '.';
                }
            }
            hex_to_screen_codes(8);
            write_line_raw(output_buffer, 0, 80);
            if (pixel_row == 0) {
                colour_address_field();
            }
        }
        mon_address += (uint32_t)BITMAP_CELLS * 8;
    }
}

/* edit_bits() lives outside this file and so cannot see the two statics below
 * it, which is why the reporting and the flush sit here rather than inside. */
static void edit_memory_bits(void) {
    if (!edit_bits()) {
        report_unmapped();
    }
    flush_sector();
}

/* Offsets within the $D640 save area of the registers worth editing, in the
 * order the R line prints them, and which of those are 16-bit pairs.  MAP, the
 * CPU port and the megabyte registers are deliberately absent: changing them
 * changes what every other address means, which wants its own command. */
static const unsigned char REGISTER_OFFSET[]
    __attribute__((section(".rodata"))) = {0x08, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07};
static const unsigned char REGISTER_IS_PAIR[] = {1, 0, 0, 0, 0, 0, 1, 0};
constexpr uint8_t REGISTER_COUNT = 8;

/* ; PC A X Y Z B SP FLAGS -- as many as given, the rest left alone.
 *
 * The KERNAL monitor prints R with a leading ';' so the line can be cursor-up
 * edited in place; read_line() here has no cursor movement, so the values are
 * typed instead.  FLAGS is a hex byte rather than the NVEBDIZC letters R shows,
 * because everything else on the line is hex too. */
void set_registers(void) {
    unsigned char index;

    for (index = 0; index < REGISTER_COUNT && parse_hex(); index++) {
        uint32_t address = 0xFFD3640UL + REGISTER_OFFSET[index];
        if (!block_write_byte(address, (unsigned char)hex_value) ||
            (REGISTER_IS_PAIR[index] &&
                !block_write_byte(address + 1, (unsigned char)(hex_value >> 8)))) {
            flush_sector();
            report_error("? FROZEN REGISTERS NOT FOUND  ERROR");
            return;
        }
    }
    if (index == 0) {
        report_error("? SYNTAX  ERROR");
        return;
    }
    flush_sector();

    /* Read them back rather than announce success, so the line on screen is
     * what the freeze slot now holds. */
    show_registers();
}

unsigned char parse_address(void) {
    // Try to read hex digits from screen_line_buffer[screen_line_offset].
    unsigned char digits = parse_hex();
    if (digits > 8) {
        write_line("? ADDRESS TOO LONG  ERROR", 0);
        recolour_last_line(SchemeError);
        write_line("(Addresses should consist of 1 - 8 hex digits).", 0);
        recolour_last_line(SchemeNotice);
        return 1;
    }
    if (digits) {
        mon_address = cpumap_typed_address(hex_value);
    }
    // No digits means continue from the previous address -- but only if the
    // rest of the line is blank.  These four commands take one address and
    // nothing after it, so leftovers are a typo: without this check `M ZZZZ`
    // parses as a bare `M` and silently shows the next block.
    while (screen_line_buffer[screen_line_offset] == ' ') {
        screen_line_offset++;
    }
    if (screen_line_offset < screen_line_length) {
        report_error("? SYNTAX  ERROR");
        return 1;
    }
    return 0;
}

/* Indexed by AssembleStatus. */
static const char* const ASSEMBLE_ERROR[] __attribute__((section(".rodata"))) = {
    [AssembleOk] = nullptr,
    [AssembleUnknownMnemonic] = "? UNKNOWN INSTRUCTION  ERROR",
    [AssembleBadOperand] = "? SYNTAX  ERROR",
    [AssembleWrongOperand] = "? SYNTAX  ERROR",
    [AssembleBranchTooFar] = "? BRANCH OUT OF RANGE  ERROR",
};

/* `A <address> <mnemonic> [operand]` assembles straight away; `A <address>`
 * on its own prompts for the instruction, as the older monitors do.  Either
 * way a fresh prompt follows at the next address until an empty line ends it. */
void assemble_memory(void) {
    uint8_t bytes[ASM_BYTES_MAX];
    uint8_t length;

    /* Not parse_address(): that rejects anything after the address, which is
     * right for the commands taking only one and wrong here, because the
     * instruction may follow on the same line.  No digits continues from the
     * previous address, as a bare A should. */
    if (parse_hex()) {
        mon_address = cpumap_typed_address(hex_value);
    }

    /* Whether the current line still carries an instruction.  `A 2000` is an
     * address and nothing else, so it prompts instead of reporting the blank
     * remainder as an unknown mnemonic.  Every line read afterwards is one. */
    bool pending = false;
    for (unsigned char i = screen_line_offset; i < screen_line_length; i++) {
        if (screen_line_buffer[i] != ' ') {
            pending = true;
            break;
        }
    }

    for (;;) {
        if (pending) {
            enum AssembleStatus status = assemble_instruction(
                mon_address, (const char*)&screen_line_buffer[screen_line_offset], bytes, &length);
            if (status != AssembleOk) {
                report_error(ASSEMBLE_ERROR[status]);
                return;
            }

            unsigned char written = 0;
            while (written < length && block_write_byte(mon_address + written, bytes[written])) {
                written++;
            }
            flush_sector();
            if (written < length) {
                report_unmapped();
                return;
            }
            /* Show what landed, which also steps to the next address.  It
             * cannot fail: the bytes were just written there. */
            (void)show_disassembly_line();
        }
        pending = true;

        /* The prompt stays on its line so the instruction is typed after it,
         * as it is on the A command that opened the loop. */
        constexpr unsigned char PROMPT_WIDTH = 10;
        lfill((long)output_buffer, ' ', 80);
        output_buffer[0] = 'A';
        format_hex(&output_buffer[2], mon_address, 7);
        write_prompt(output_buffer, PROMPT_WIDTH);

        char typed = read_line((char*)screen_line_buffer, 80 - PROMPT_WIDTH, PROMPT_WIDTH);
        next_line();
        if (!typed) {
            return;
        }
        screen_line_buffer[79] = 0;
        screen_line_offset = 0;
        screen_line_length = (unsigned char)strlen((char*)screen_line_buffer);
    }
}

void freeze_monitor(void) {

    setup_screen();

    // Flush input buffer
    while (ASCIIKEY) {
        ASCIIKEY = 0;
    }

    show_registers();

    /* Start where the program stopped, so a bare M or D shows something worth
     * looking at rather than address zero. */
    if (frozen_program_counter != 0xffffffffUL) {
        mon_address = frozen_program_counter;
    }

    while (1) {
        read_line((char*)screen_line_buffer, 80, 0);
        screen_line_buffer[79] = 0;
        write_line((char*)screen_line_buffer, 0);

        // Skip initial char for parsing routines
        screen_line_offset = 1;
        screen_line_length = (unsigned char)strlen((char*)screen_line_buffer);

        // Command syntax purposely matches that of the Matrix Mode / UART monitor to avoid
        // confusion
        switch (screen_line_buffer[0]) {
            /* Alphabetical, matching the order the footer lists them in. */
            case 'a':
            case 'A':
            case '.': // the KERNAL monitor's alias, so a listing can be retyped
                assemble_memory();
                break;
            case 'b':
            case 'B':
                // Bits: B start
                if (parse_address()) {
                    break;
                }
                edit_memory_bits();
                break;
            case 'c':
            case 'C':
                // Compare: C start end other
                compare_memory();
                break;
            case 'd':
            case 'D':
                // Disassemble; a bare D continues from where the last one
                // stopped, as M does.
                if (parse_address()) {
                    break;
                }
                show_disassembly();
                break;
            case 'f':
            case 'F':
                // Fill memory: F start end value
                fill_memory();
                break;
            case 'h':
            case 'H':
                // Hunt memory: H start end byte [byte...]
                hunt_memory();
                break;
            case 'm':
            case 'M':
                // Display memory
                if (parse_address()) {
                    break;
                }
                show_memory();
                break;
            case 'p':
            case 'P':
                // Pixels: P start
                if (parse_address()) {
                    break;
                }
                show_bitmaps();
                break;
            case 'r':
            case 'R':
                // Display register values
                show_registers();
                break;
            case 's':
            case 'S':
                // Set memory values
                if (parse_address()) {
                    break;
                }
                set_memory();
                break;
            case 't':
            case 'T':
                // Transfer: T start end destination
                transfer_memory();
                break;
            case 'x':
            case 'X':
                // Exit monitor
                // Return screen to normal
                VICIV.ctrlc = (VICIV.ctrlc & 0xa8) | 0x00;
                VICIV.addr = 0x15; // VIC-II hot register, resets display settings
                VICIV.ctrl2 = 0xC8;
                CIA2.pra = CIA2.pra | 3;          // video bank 0
                VICIV.ctrlb = VICIV.ctrlb & 0x7f; // 40 columns
                return;
            case ';':
                // Modify the frozen registers, as the KERNAL monitor's ; does
                set_registers();
                break;
            case 0:
                // Empty line.  Listed after the commands because it is not one,
                // and because sitting beside the assemble stub made the two
                // identical branches.
                break;
            default:
                write_line("Unknown command.", 0);
                recolour_last_line(SchemeError);
                break;
        }
    }
}
