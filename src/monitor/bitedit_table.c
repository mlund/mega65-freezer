/* The I/O register names and the row they render.  The database is IOMAP.M65 on
 * the card, reached only through iomap_byte(), so this file needs no MEGA65 and
 * test/verify_bitedit.py builds it for the host over a buffer. */

#include "bitedit_table.h"

#include "format.h"

#include <string.h>

/* Shift-Q in the C64 ROM charset the screen actually renders with, which is not
 * the embedded ascii8x8.bin: see the charset note. */
constexpr uint8_t GLYPH_BIT = 0x51;

/* The NAME column, used only while rendering a row. */
constexpr uint8_t BITEDIT_NAME_COLUMN = 10;

/* Record header: address low, address high, total length, chip and flags. */
constexpr uint8_t RECORD_HEADER = 4;
constexpr uint8_t FLAG_CHIP_MASK = 0x1F;
constexpr uint8_t FLAG_HAS_FIELDS = 0x20;
constexpr uint8_t FLAG_HAS_NAME = 0x40;
constexpr uint8_t FLAG_HAS_TEXT = 0x80;

/* File header: "M65I", format version, chip count, then three 16-bit offsets. */
constexpr uint8_t HEADER_BYTES = 12;
constexpr uint8_t FORMAT_VERSION = 1;

static uint16_t record_start;
static uint16_t text_start;
static uint16_t file_end;

static uint8_t table_byte(uint16_t offset) {
    return iomap_byte(offset);
}

static uint16_t table_word(uint16_t offset) {
    return (uint16_t)(table_byte(offset) | (table_byte((uint16_t)(offset + 1)) << 8));
}

bool bitedit_open(void) {
    record_start = text_start = file_end = 0;

    if (table_byte(0) != 'M' || table_byte(1) != '6' || table_byte(2) != '5' ||
        table_byte(3) != 'I') {
        return false;
    }
    if (table_byte(4) != FORMAT_VERSION) {
        return false;
    }
    const uint16_t records = table_word(6);
    const uint16_t text = table_word(8);
    const uint16_t end = table_word(10);
    if (records < HEADER_BYTES || records >= text || text > end) {
        return false;
    }
    /* Set only once the header holds together.  A half-accepted header would
     * leave record_start non-zero, and bitedit_lookup() reads that as "there is
     * a table" and walks a file that is not there. */
    record_start = records;
    text_start = text;
    file_end = end;
    return true;
}

/* $FFD0000, $FFD1000, $FFD2000 and $FFD3000 are the same registers seen through
 * four I/O personalities; the freezer uses $FFD3xxx.  Only the low twelve bits
 * pick the register, so all four fold to one lookup. */
uint16_t bitedit_io_address(uint32_t address) {
    if ((address & 0xFFFC000UL) != 0xFFD0000UL) {
        return 0;
    }
    return (uint16_t)(0xD000 | (address & 0x0FFF));
}

uint16_t bitedit_string(uint16_t offset, char* out, uint16_t limit) {
    uint16_t length = 0;

    if (offset == BITEDIT_NO_TEXT) {
        return 0;
    }
    while (length < limit) {
        uint8_t code = table_byte((uint16_t)(offset + length));
        out[length] = (char)(code & 0x7F);
        length++;
        if (code & 0x80) {
            break;
        }
    }
    return length;
}

/* Step over one stored string.  Bounded by the file rather than by trusting a
 * terminator to arrive: a truncated card file reads as whatever was already at
 * $40000, and an unbounded scan there wraps past $FFFF and never returns. */
static uint16_t skip_string(uint16_t offset) {
    while (offset < file_end && !(table_byte(offset) & 0x80)) {
        offset++;
    }
    return (uint16_t)(offset + 1);
}

/* The chip names are concatenated in index order, so reaching one means
 * stepping over those before it -- fifteen short names, and only on the rows
 * where the chip changes. */
uint16_t bitedit_chip_name(uint8_t index) {
    uint16_t at = HEADER_BYTES;
    while (index--) {
        at = skip_string(at);
    }
    return at;
}

bool bitedit_lookup(uint16_t io_address, RegisterInfo* out, uint16_t* scan) {
    memset(out, 0, sizeof(*out));

    if (!record_start) {
        return false;
    }
    /* Records are sorted, so the walk stops as soon as it passes the target.
     * The length byte is what makes each step one addition rather than a
     * decode of the record it is skipping. */
    uint16_t offset = *scan ? *scan : record_start;
    while (offset < text_start) {
        uint16_t address = table_word(offset);
        if (address > io_address) {
            *scan = offset;
            return false;
        }
        const uint8_t length = table_byte((uint16_t)(offset + 2));
        if (length < RECORD_HEADER) {
            /* A record cannot be shorter than its own header, so this is the
             * end of a file that has been cut short.  Without the test the walk
             * would step by zero for ever. */
            break;
        }
        if (address == io_address) {
            *scan = offset;
            const uint8_t flags = table_byte((uint16_t)(offset + 3));
            uint16_t at = (uint16_t)(offset + RECORD_HEADER);
            out->chip = flags & FLAG_CHIP_MASK;

            if (flags & FLAG_HAS_FIELDS) {
                /* Eight 3-bit indices, bit 7 of the register in the topmost. */
                uint32_t packed = ((uint32_t)table_byte(at) << 16) |
                    ((uint32_t)table_byte((uint16_t)(at + 1)) << 8) |
                    table_byte((uint16_t)(at + 2));
                for (uint8_t bit = 0; bit < 8; bit++) {
                    out->bit_field[bit] = (uint8_t)(packed & 7);
                    packed >>= 3;
                }
                at += 3;
            }
            if (flags & FLAG_HAS_TEXT) {
                out->text = (uint16_t)(text_start + table_word(at));
                at += 2;
            }
            if (flags & FLAG_HAS_NAME) {
                out->name = at;
                at = skip_string(at);
            }
            const uint16_t end = (uint16_t)(offset + length);
            while (at < end && out->field_count < BITEDIT_MAX_FIELDS) {
                const uint16_t where = table_word(at);
                out->field_text[out->field_count] =
                    where == 0xFFFF ? BITEDIT_NO_TEXT : (uint16_t)(text_start + where);
                at += 2;
                out->field_name[out->field_count++] = at;
                at = skip_string(at);
            }
            return true;
        }
        offset = (uint16_t)(offset + length);
    }
    *scan = offset;
    return false;
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
        bitedit_string(info->name, &out[BITEDIT_NAME_COLUMN], BITEDIT_NAME_WIDTH);
    } else if (info->field_count && info->chip != previous_chip) {
        /* Only where it changes: most registers with named bits have no
         * mnemonic of their own, so printing the chip on each would fill the
         * column with the same word. */
        bitedit_string(
            bitedit_chip_name(info->chip), &out[BITEDIT_NAME_COLUMN], BITEDIT_NAME_WIDTH);
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
            bitedit_string(info->field_name[field], cell, BITEDIT_BIT_WIDTH);
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

/* Nine cells in a ring: the value field sits one past bit 7, so a step either
 * way is a count with a wrap. */
uint8_t bitedit_cursor_left(uint8_t cell) {
    return cell == BITEDIT_CELL_VALUE ? 0 : (uint8_t)(cell + 1);
}

uint8_t bitedit_cursor_right(uint8_t cell) {
    return cell == 0 ? BITEDIT_CELL_VALUE : (uint8_t)(cell - 1);
}

/* Forced inline because LTO measured 15 bytes worse leaving it out of line, and
 * on the target there is only the one caller.  It lives here rather than beside
 * that caller so the host test can drive it. */
__attribute__((always_inline)) uint8_t bitedit_wrap(
    const char* body, uint16_t length, uint16_t at, uint8_t room) {
    const uint16_t remaining = (uint16_t)(length - at);
    if (remaining <= room) {
        return (uint8_t)remaining;
    }
    /* Backwards, so the first space found is the last one that fits. */
    const char* from = &body[at];
    for (uint8_t i = room; i--;) {
        if (from[i] == ' ') {
            return (uint8_t)(i + 1);
        }
    }
    /* A word wider than the row has to be cut somewhere. */
    return room;
}
