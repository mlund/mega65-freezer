/* The I/O register names and the row they render.  Pure by construction --
 * static table in, caller's buffer out -- so test/verify_bitedit.py builds this
 * file for the host and checks the record walk without an emulator. */

#include "bitedit_table.h"

#include "format.h"

#include <string.h>

/* Every zero-page byte the LTO allocator spends on this table is one the
 * imaginary registers do not get.  Guarded because Mach-O rejects a bare
 * section name, and the host build has no such section anyway. */
#if defined(__mos__)
#define IOMAP_RODATA __attribute__((section(".rodata")))
#else
#define IOMAP_RODATA
#endif

#include "iomap_names.inc"

/* Shift-Q in the C64 ROM charset the screen actually renders with, which is not
 * the embedded ascii8x8.bin: see the charset note. */
constexpr uint8_t GLYPH_BIT = 0x51;

/* The NAME column, used only while rendering a row. */
constexpr uint8_t BITEDIT_NAME_COLUMN = 10;
constexpr uint8_t BITEDIT_NAME_WIDTH = 8;

/* Record header: address low, address high, total length, chip and flags. */
constexpr uint8_t RECORD_HEADER = 4;
constexpr uint8_t FLAG_CHIP_MASK = 0x1F;
constexpr uint8_t FLAG_HAS_FIELDS = 0x20;
constexpr uint8_t FLAG_HAS_NAME = 0x40;

/* $FFD0000, $FFD1000, $FFD2000 and $FFD3000 are the same registers seen through
 * four I/O personalities; the freezer uses $FFD3xxx.  Only the low twelve bits
 * pick the register, so all four fold to one lookup. */
uint16_t bitedit_io_address(uint32_t address) {
    if ((address & 0xFFFC000UL) != 0xFFD0000UL) {
        return 0;
    }
    return (uint16_t)(0xD000 | (address & 0x0FFF));
}

static uint8_t copy_name(char* out, const uint8_t* name, uint8_t limit) {
    uint8_t length = 0;
    while (length < limit) {
        uint8_t code = name[length];
        out[length] = (char)(code & 0x7F);
        length++;
        if (code & 0x80) {
            break;
        }
    }
    return length;
}

static const uint8_t* skip_name(const uint8_t* name) {
    while (!(*name & 0x80)) {
        name++;
    }
    return name + 1;
}

/* The chip names are concatenated in index order, so reaching one means
 * stepping over those before it.  29 short names, and only on the rows where
 * the chip changes. */
static const uint8_t* chip_name(uint8_t index) {
    const uint8_t* at = IOMAP_CHIPS;
    while (index--) {
        at = skip_name(at);
    }
    return at;
}

bool bitedit_lookup(uint16_t io_address, RegisterInfo* out, uint16_t* scan) {
    memset(out, 0, sizeof(*out));

    /* Records are sorted, so the walk stops as soon as it passes the target.
     * The length byte is what makes each step one addition rather than a
     * decode of the record it is skipping. */
    uint16_t offset = *scan;
    while (offset < IOMAP_TABLE_BYTES) {
        const uint8_t* record = &IOMAP_TABLE[offset];
        uint16_t address = (uint16_t)(record[0] | (record[1] << 8));
        if (address > io_address) {
            *scan = offset;
            return false;
        }
        if (address == io_address) {
            *scan = offset;
            const uint8_t flags = record[3];
            const uint8_t* at = record + RECORD_HEADER;
            out->chip = flags & FLAG_CHIP_MASK;

            if (flags & FLAG_HAS_FIELDS) {
                /* Eight 3-bit indices packed big-end first, bit 7 of the
                 * register in the topmost field. */
                uint32_t packed = ((uint32_t)at[0] << 16) | ((uint32_t)at[1] << 8) | at[2];
                for (uint8_t bit = 0; bit < 8; bit++) {
                    out->bit_field[bit] = (uint8_t)(packed & 7);
                    packed >>= 3;
                }
                at += 3;
            }
            if (flags & FLAG_HAS_NAME) {
                out->name = at;
                at = skip_name(at);
            }
            const uint8_t* end = record + record[2];
            while (at < end && out->field_count < BITEDIT_MAX_FIELDS) {
                out->field[out->field_count++] = at;
                at = skip_name(at);
            }
            return true;
        }
        offset = (uint16_t)(offset + record[2]);
    }
    *scan = offset;
    return false;
}

uint8_t bitedit_cursor_left(uint8_t cell) {
    return cell == BITEDIT_CELL_VALUE ? 0 : (uint8_t)(cell + 1);
}

uint8_t bitedit_cursor_right(uint8_t cell) {
    return cell == 0 ? BITEDIT_CELL_VALUE : (uint8_t)(cell - 1);
}

/* Character lists rather than string literals: these are screen codes of a fixed
 * width with no terminator, the same reason disasm_tables.inc spells its
 * mnemonics out. */
static const char LABEL_ADDR[] = {'A' - 0x40, 'D' - 0x40, 'D' - 0x40, 'R' - 0x40};
static const char LABEL_NAME[] = {'N' - 0x40, 'A' - 0x40, 'M' - 0x40, 'E' - 0x40};
static const char LABEL_VAL[] = {'V' - 0x40, 'A' - 0x40, 'L' - 0x40};

static void put_label(char* out, const char* label, uint8_t length) {
    for (uint8_t i = 0; i < length; i++) {
        out[i] = label[i];
    }
}

void bitedit_header(char* out) {
    memset(out, ' ', BITEDIT_ROW_WIDTH);
    put_label(&out[1], LABEL_ADDR, sizeof(LABEL_ADDR));
    put_label(&out[BITEDIT_NAME_COLUMN], LABEL_NAME, sizeof(LABEL_NAME));
    put_label(&out[BITEDIT_VALUE_COLUMN], LABEL_VAL, sizeof(LABEL_VAL));
    for (uint8_t bit = 0; bit < 8; bit++) {
        out[BITEDIT_BIT_COLUMN + bit * BITEDIT_BIT_STRIDE] = (char)(0x30 + (7 - bit));
    }
}

uint8_t bitedit_render(
    char* out, uint32_t address, uint8_t value, const RegisterInfo* info, uint8_t previous_chip) {
    memset(out, ' ', BITEDIT_ROW_WIDTH);

    out[0] = ':';
    format_hex(&out[1], (long)address, BITEDIT_ADDRESS_WIDTH - 1);

    out[BITEDIT_VALUE_COLUMN] = (char)nybl_to_screen((uint8_t)(value >> 4));
    out[BITEDIT_VALUE_COLUMN + 1] = (char)nybl_to_screen(value);

    uint8_t chip = previous_chip;
    if (info->name) {
        copy_name(&out[BITEDIT_NAME_COLUMN], info->name, BITEDIT_NAME_WIDTH);
    } else if (info->field_count && info->chip != previous_chip) {
        /* Only where it changes: 119 of the 155 registers with named bits have
         * no mnemonic of their own, so printing the chip on each would fill the
         * column with the same word. */
        copy_name(&out[BITEDIT_NAME_COLUMN], chip_name(info->chip), BITEDIT_NAME_WIDTH);
        chip = info->chip;
    }

    for (uint8_t bit = 0; bit < 8; bit++) {
        char* cell = &out[BITEDIT_BIT_COLUMN + (7 - bit) * BITEDIT_BIT_STRIDE];
        uint8_t field = info->bit_field[bit];

        if (field >= info->field_count) {
            *cell = (char)GLYPH_BIT;
            continue;
        }
        /* A field wider than one bit is named once, over its highest bit, and
         * the rest of its span left blank -- the User Guide's register tables
         * merge the cell the same way, and repeating the name reads as several
         * separate flags.  Blank is unambiguous because an unnamed bit shows a
         * circle, so nothing else can leave a cell empty. */
        if (bit == 7 || info->bit_field[bit + 1] != field) {
            copy_name(cell, info->field[field], BITEDIT_BIT_WIDTH);
        }
    }
    return chip;
}

uint8_t bitedit_toggle(uint8_t value, uint8_t bit) {
    return (uint8_t)(value ^ (uint8_t)(1u << (bit & 7)));
}

/* Shifts in from the right, so two digits replace the byte and one leaves the
 * low nibble as typed -- the way a calculator takes a value, with no separate
 * commit. */
uint8_t bitedit_hex_digit(uint8_t value, uint8_t digit) {
    return (uint8_t)((value << 4) | (digit & 0x0F));
}
