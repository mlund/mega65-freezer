/*
  Memory monitor for the freezer.

  We know where the freeze regions are from freeze_region_list[],
  so we can convert requested addresses into offsets in the freeze slot.

  We can also allow viewing/modification of the raw freeze slot data itself.

*/

#include "disasm.h"
#include "fdisk_fat32.h"
#include "fdisk_hal.h"
#include "fdisk_memory.h"
#include "fdisk_screen.h"
#include "fdisk_screen_monitor.h"
#include "freezer.h"
#include "mega65_regs.h"

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

unsigned char screen_line = 0;
unsigned char screen_line_buffer[80];
unsigned char screen_line_length = 0;
unsigned char screen_line_offset = 0;

uint32_t hex_value = 0;
uint32_t mon_address = 0;

char output_buffer[80];

unsigned char mon_sector[512];
uint32_t mon_sector_num = 0xffffffffUL;

/* Frozen CPU state that M and D need in order to talk about the same addresses
 * the program did, cached by show_registers().  The PC here is already resolved
 * to 28 bits; the sentinel keeps an unread PC from matching any real address. */
uint32_t frozen_program_counter = 0xffffffffUL;
static uint16_t frozen_map_lo = 0;
static uint16_t frozen_map_hi = 0;
static unsigned char frozen_map_lo_megabyte = 0;
static unsigned char frozen_map_hi_megabyte = 0;
static unsigned char frozen_cpu_port = 0;

/* Translate an address as the frozen CPU saw it into the 28-bit address M and D
 * take.  The MAP register offsets 8KB blocks: MAPLO covers $0000-$7FFF and
 * MAPHI $8000-$FFFF, each holding a selection nibble in its top four bits whose
 * bit N enables block N of that half, and an offset in the remaining three
 * nibbles scaled by 256.  The sum wraps within the 20 bits the 4510 MAP spans.
 * (MEGA65 Book, "The MAP Register".) */
uint32_t resolve_cpu_address(uint16_t cpu_address) {
    unsigned char block = (unsigned char)(cpu_address >> 13);
    bool lower_half = block < 4;
    uint16_t map = lower_half ? frozen_map_lo : frozen_map_hi;
    unsigned char megabyte = lower_half ? frozen_map_lo_megabyte : frozen_map_hi_megabyte;

    if (((map >> 12) & (1U << (block & 3))) != 0) {
        /* The offset addition wraps inside its megabyte, which the 45GS02's
         * own megabyte register then places in the 28-bit space. */
        return ((uint32_t)megabyte << 20) |
            (((uint32_t)cpu_address + (((uint32_t)map & 0x0FFF) << 8)) & 0xFFFFFUL);
    }

    /* Not mapped, so C64-style banking still decides: $01 bit 1 banks the
     * KERNAL in and bits 0+1 together the BASIC ROM, which the MEGA65 holds at
     * $2E000 and $2A000 -- both $20000 above their 16-bit window. */
    if (cpu_address >= 0xE000U && (frozen_cpu_port & 0x02) != 0) {
        return 0x20000UL + cpu_address;
    }
    if (cpu_address >= 0xA000U && cpu_address < 0xC000U && (frozen_cpu_port & 0x03) == 0x03) {
        return 0x20000UL + cpu_address;
    }
    return cpu_address;
}

/* Bring the sector holding `freeze_slot_offset` into mon_sector.  SD reads
 * dominate the cost of every command here, so the tag check guards all three
 * callers rather than each keeping its own copy of the same test. */
static void cache_sector(uint32_t freeze_slot_offset) {
    if (mon_sector_num != (freeze_slot_offset >> 9)) {
        mon_sector_num = (freeze_slot_offset >> 9);
        sdcard_readsector(freeze_slot_start_sector + mon_sector_num);
        lcopy((long)sector_buffer, (long)mon_sector, 512);
    }
}

/* Fetch one byte of frozen memory.  The check is per byte because an
 * instruction can begin in one sector and end in the next; show_memory_line()
 * gets away with one check per line only because its runs are 16-byte aligned. */
bool disasm_read_byte(uint32_t address, uint8_t* value) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(address);
    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        return false;
    }
    cache_sector(freeze_slot_offset);
    *value = mon_sector[freeze_slot_offset & 0x1ff];
    return true;
}

void show_memory_line(uint32_t addr) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);
    unsigned char i;

    lfill((long)output_buffer, 0, 80);
    output_buffer[0] = ':';
    format_hex(&output_buffer[1], addr, 7);
    output_buffer[8] = ' ';

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        // Memory that isn't saved
        for (i = 0; i < 65; i++)
            output_buffer[9 + i] =
                "<UNMAPPED OR UNFROZEN MEMORY>                                    "[i] & 0x3f;
        output_buffer[9] = '<';
        output_buffer[9 + 28] = '>';
    } else {
        cache_sector(freeze_slot_offset);
        // Two spaces before character rendering of block
        output_buffer[8 + 16 * 3 + 0] = ' ';
        output_buffer[8 + 16 * 3 + 1] = ' ';
        for (i = 0; i < 16; i++) {
            unsigned char b = mon_sector[(i + freeze_slot_offset) & 0x1ff];
            // Space before hex
            output_buffer[8 + i * 3] = ' ';
            // hex digits
            format_hex(&output_buffer[8 + 1 + i * 3], b, 2);
            // C64 character rendering of the same byte
            output_buffer[8 + 16 * 3 + 2 + i] = b;
        }
    }
    // Convert hex back to C64 screen codes
    constexpr uint8_t HEX_FIELD_END = 8 + 16 * 3;
    for (i = 0; i < HEX_FIELD_END; i++) {
        if (output_buffer[i] >= 'A' && output_buffer[i] <= 'F')
            output_buffer[i] &= 0x0f;
    }

    write_line_raw(output_buffer, 0, 8 + 16 * 3 + 2 + 16);
}

void show_memory(void) {
    unsigned char i;
    for (i = 0; i < 16; i++) {
        show_memory_line(mon_address);
        mon_address += 16;
    }
}

/* A screenful, matching show_memory()'s sixteen lines. */
constexpr uint8_t DISASSEMBLY_LINES = 16;

/* Colour-RAM indices for the fields of a disassembled line.  Chosen to stay
 * legible on a dark background: MONITOR never calls set_palette(), so it
 * inherits whatever palette the frozen program left behind and no exact hue can
 * be relied on. */
constexpr uint8_t COLOUR_ADDRESS = 3; /* cyan */
constexpr uint8_t COLOUR_BYTES = 12;  /* medium grey: deliberately dim */
constexpr uint8_t COLOUR_OPERAND = 5; /* green */
/* Indexed by DisasmMnemonicClass: plain, control flow, MEGA65-only. */
static const unsigned char MNEMONIC_CLASS_COLOUR[] = {1, 7, 13};

/* Colour the fields of the line just written.  `attribute` is OR'd into every
 * field so a whole-line marker such as ATTRIB_REVERSE leaves no gaps; the
 * mnemonic is coloured across its padded field for the same reason. */
static void colour_disassembly_line(const DisassemblyLayout* layout, unsigned char attribute) {
    unsigned char mnemonic_colour = MNEMONIC_CLASS_COLOUR[layout->mnemonic_class];
    unsigned char operand_column = layout->mnemonic_column + DISASM_MNEMONIC_FIELD_WIDTH;

    recolour_last_line_segment(0, DISASM_BYTE_COLUMN, COLOUR_ADDRESS | attribute);
    recolour_last_line_segment(
        DISASM_BYTE_COLUMN, layout->mnemonic_column - DISASM_BYTE_COLUMN, COLOUR_BYTES | attribute);
    recolour_last_line_segment(
        layout->mnemonic_column, DISASM_MNEMONIC_FIELD_WIDTH, mnemonic_colour | attribute);
    if (layout->text_length > operand_column) {
        recolour_last_line_segment(
            operand_column, layout->text_length - operand_column, COLOUR_OPERAND | attribute);
    }
}

void show_disassembly(void) {
    char text[DISASM_TEXT_MAX];
    DisassemblyLayout layout;
    unsigned char line;

    for (line = 0; line < DISASSEMBLY_LINES; line++) {
        if (!disassemble_instruction(mon_address, text, &layout)) {
            write_line("? UNMAPPED OR UNFROZEN ADDRESS  ERROR", 0);
            recolour_last_line(2);
            /* Step past the hole so a repeated D can walk out of it, as M does.
             * One byte, because without a decode there is no instruction
             * boundary to trust. */
            mon_address++;
            return;
        }
        /* The saved PC is 16 bits, so it names a CPU address whose physical
         * bank depends on MAP and $01 state this code does not decode.  Marking
         * only bank 0 gives up the highlight on a banked PC rather than risk
         * reverse-videoing an unrelated line that happens to share the low 16
         * bits -- ROM at $3FA23 and RAM at $0FA23 both match $FA23. */
        unsigned char attribute = (mon_address == frozen_program_counter) ? ATTRIB_REVERSE : 0;

        write_line_len(text, 0, layout.text_length);
        colour_disassembly_line(&layout, attribute);
        mon_address += layout.length;
    }
}

void set_memory() {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(mon_address);
    unsigned char i;

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        write_line("? UNMAPPED OR UNFROZEN ADDRESS  ERROR", 0);
        recolour_last_line(2);
        return;
    } else {
        cache_sector(freeze_slot_offset);

        // Get position within sector
        i = 0;
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
                                (screen_line_buffer[screen_line_offset] < 'z')))
                            mon_sector[(freeze_slot_offset + i) & 0x1ff] =
                                screen_line_buffer[screen_line_offset++] & 0x1f;
                        else
                            mon_sector[(freeze_slot_offset + i) & 0x1ff] =
                                screen_line_buffer[screen_line_offset++];
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
                            mon_sector[(freeze_slot_offset + i) & 0x1ff] << 4;
                        mon_sector[(freeze_slot_offset + i) & 0x1ff] |=
                            char_to_hex(screen_line_buffer[screen_line_offset++]);
                    }
                    i++;
                    break;
                default:
                    write_line("? SYNTAX  ERROR", 0);
                    recolour_last_line(2);
                    return;
            }
        }

        // Write changes back
        lcopy((long)mon_sector, (long)sector_buffer, 512);
        sdcard_writesector(freeze_slot_start_sector + mon_sector_num, 0);

        // After writing memory values, redisplay the modified region
        mon_sector_num = -1;
        show_memory();
    }
}

static const char REG_DESC_LINE[] =
    "PC   IRQ  NMI  A  X  Y  Z  B  SP   FLAGS    $01   MAPLO   MAPHI   PC28";
#define REGLINE_PC 0
#define REGLINE_IRQ 5
#define REGLINE_NMI 10
#define REGLINE_A 15
#define REGLINE_X 18
#define REGLINE_Y 21
#define REGLINE_Z 24
#define REGLINE_B 27
#define REGLINE_SP 30
#define REGLINE_FLAGS 35
#define REGLINE_01 44
#define REGLINE_MAPLO 50
#define REGLINE_MAPHI 58
#define REGLINE_PC28 66
void show_registers(void) {
    // Get hypervisor register backup area
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(0xFFD3640U);
    unsigned short value;

    lfill((long)output_buffer, ' ', 80);

    /* Test the sentinel before shifting: 0xFFFFFFFF >> 9 is 0x7FFFFF, so a
     * shifted value can never equal it and the guard would never fire. */
    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        write_line("? FROZEN REGISTERS NOT FOUND  ERROR", 0);
        recolour_last_line(2);
    } else {
        freeze_slot_offset = freeze_slot_offset >> 9L;
        sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);

        // Now show registers: First the description line
        write_line(REG_DESC_LINE, 0);

        // Now prepare the line of actual register values

        // $D640-$D67F is frozen as a single piece, so the offsets are $00, not $40 from the
        // beginning

        /* Cache the mapping state before the PC, which is resolved through it.
         * $D64A/$D64C hold the selection nibble and the offset's high nibble,
         * $D64B/$D64D the offset's low byte -- high byte first, not the little
         * endian the rest of this save area uses (gs4510.vhdl:4926). */
        frozen_map_lo = (uint16_t)(sector_buffer[0x0A] << 8) | sector_buffer[0x0B];
        frozen_map_hi = (uint16_t)(sector_buffer[0x0C] << 8) | sector_buffer[0x0D];
        frozen_map_lo_megabyte = sector_buffer[0x0E];
        frozen_map_hi_megabyte = sector_buffer[0x0F];
        frozen_cpu_port = sector_buffer[0x11];

        // PC
        value = sector_buffer[0x08] + (sector_buffer[0x09] << 8);
        format_hex(&output_buffer[REGLINE_PC], value, 4);

        /* The 28-bit address that PC actually reached, which is what M and D
         * accept -- the 16-bit value above names a window, not a location. */
        frozen_program_counter = resolve_cpu_address(value);
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
        format_hex(&output_buffer[REGLINE_MAPLO], frozen_map_lo, 4);
        value = sector_buffer[0x0E];
        output_buffer[REGLINE_MAPLO + 4] = '/';
        format_hex(&output_buffer[REGLINE_MAPLO + 5], value, 2);

        // MAPHI
        format_hex(&output_buffer[REGLINE_MAPHI], frozen_map_hi, 4);
        value = sector_buffer[0x0F];
        output_buffer[REGLINE_MAPHI + 4] = '/';
        format_hex(&output_buffer[REGLINE_MAPHI + 5], value, 2);

        write_line(output_buffer, 0);
    }
}

unsigned char parse_hex(void) {
    unsigned char digits = 0;
    hex_value = 0;
    while (1) {
        switch (screen_line_buffer[screen_line_offset]) {
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
                hex_value = hex_value << 4;
                hex_value |= screen_line_buffer[screen_line_offset] & 0xf;
                digits++;
                screen_line_offset++;
                break;
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
                hex_value = hex_value << 4;
                hex_value |= 9 + (screen_line_buffer[screen_line_offset] & 0xf);
                digits++;
                screen_line_offset++;
                break;
            case ' ': // Allow leading spaces.
                if (digits == 0) {
                    screen_line_offset++;
                    break;
                }
            default:
                return digits;
        }
    }
}

unsigned char parse_address(void) {
    // Try to read hex digits from screen_line_buffer[screen_line_offset].
    unsigned char digits = parse_hex();
    if (digits > 7) {
        write_line("? ADDRESS TOO LONG  ERROR", 0);
        recolour_last_line(2);
        write_line("(Addresses should consist of 1 - 7 hex digits).", 0);
        recolour_last_line(7);
        return 1;
    }
    if (!digits) {
        // No digits, so use previous address
    } else {
        // Use the supplied address
        mon_address = hex_value;
    }
    return 0;
}

void freeze_monitor(void) {

    setup_screen();

    // Flush input buffer
    while (ASCIIKEY)
        ASCIIKEY = 0;

    show_registers();

    /* Start where the program stopped, so a bare M or D shows something worth
     * looking at rather than address zero. */
    if (frozen_program_counter != 0xffffffffUL) {
        mon_address = frozen_program_counter;
    }

    while (1) {
        read_line((char*)screen_line_buffer, 80);
        screen_line_buffer[79] = 0;
        write_line((char*)screen_line_buffer, 0);

        // Skip initial char for parsing routines
        screen_line_offset = 1;
        screen_line_length = strlen((char*)screen_line_buffer);

        // Command syntax purposely matches that of the Matrix Mode / UART monitor to avoid
        // confusion
        switch (screen_line_buffer[0]) {
            case 0:
                // empty line - nothing to do
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
            case 'm':
            case 'M':
                // Display memory
                if (parse_address())
                    break;
                show_memory();
                break;
            case 'd':
            case 'D':
                // Disassemble; a bare D continues from where the last one
                // stopped, as M does.
                if (parse_address())
                    break;
                show_disassembly();
                break;
            case 'a': // assemble
            case 'A':
                // Not implemented.
                break;
            case 'r':
            case 'R':
                // Display register values
                show_registers();
                break;
            case 'f': // fill memory
            case 'F':
            case 'h': // search (hunt) memory
            case 'H':
                // Not implemented.
                break;
            case 's':
            case 'S':
                // Set memory values
                if (parse_address())
                    break;
                set_memory();
                break;
            default:
                write_line("Unknown command.", 0);
                recolour_last_line(0x02);
                break;
        }
    }
}
