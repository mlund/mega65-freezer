/* Host driver for the bit editor's table, used only by test/verify_bitedit.py.
 *
 * bitedit_table.c reaches the database only through iomap_byte(), so on the
 * host that becomes a read from IOMAP.M65 held in memory and the whole decoder
 * runs with no MEGA65.  The source is included rather than linked because
 * `walk` steps the raw record stream to prove the length bytes are right, and
 * the file's geometry is private to the translation unit.
 *
 * Takes the path to IOMAP.M65 as its only argument.  Commands on stdin, one per
 * line; addresses and values are hex, everything else decimal, and rendered
 * rows come back as hex screen codes so the checker sees what would reach the
 * screen:
 *   lookup <addr>            -> "<chip> <fields> <name|->", 8 bit indices,
 *                               then one line of space-separated field names
 *   text <addr> <bit|-1>     -> the description, or "-"
 *   cursor <cell>            -> "<left> <right>", one step each way
 *   wrap <room> <text>       -> "=" and one wrapped row per line, then "."
 *   row <addr> <val> <prev>  -> "<chip> <hex bytes>"
 *   header                   -> hex bytes
 *   walk                     -> "<addr> <length>" per record
 *   toggle <val> <bit>       -> the new value
 *   hex <val> <digit>        -> the new value
 */

#include "bitedit_table.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t database[64 * 1024];
static size_t database_bytes;

uint8_t iomap_byte(uint16_t offset) {
    /* Zero past the end, as reading past the card's copy would be. */
    return offset < database_bytes ? database[offset] : 0;
}

/* One stored string back to ASCII, so the checker can compare it with what the
 * generator's own abbreviation produced. */
static void readable(char* out, uint16_t offset, uint8_t limit) {
    uint8_t length = bitedit_string(offset, out, limit);
    /* Screen codes $00-$1F are ASCII $40-$5F: `@`, the letters, and the
     * brackets.  Leaving $1B or $1D raw would emit a control character. */
    for (uint8_t i = 0; i < length; i++) {
        unsigned char code = (unsigned char)out[i];
        out[i] = code <= 0x1F ? (char)(code + 0x40) : (char)code;
    }
    out[length] = '\0';
}

static void put_codes(const char* row) {
    for (uint8_t i = 0; i < BITEDIT_ROW_WIDTH; i++) {
        printf("%02x", (unsigned char)row[i]);
    }
    printf("\n");
}

int main(int argc, char** argv) {
    char line[600];
    char row[BITEDIT_ROW_WIDTH];

    if (argc < 2) {
        fprintf(stderr, "usage: %s IOMAP.M65\n", argv[0]);
        return 2;
    }
    FILE* file = fopen(argv[1], "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    database_bytes = fread(database, 1, sizeof(database), file);
    fclose(file);

    if (!bitedit_open()) {
        fprintf(stderr, "%s is not an IOMAP database\n", argv[1]);
        return 2;
    }

    while (fgets(line, sizeof(line), stdin)) {
        unsigned a, b, c;
        int bit;

        if (sscanf(line, "lookup %x", &a) == 1) {
            RegisterInfo info;
            uint16_t scan = 0;
            if (!bitedit_lookup((uint16_t)a, &info, &scan)) {
                /* Always three lines, so the checker can index replies by a
                 * fixed stride rather than parsing to find the boundaries. */
                printf("none\n\n\n");
                continue;
            }
            char name[BITEDIT_NAME_WIDTH + 1] = {0};
            if (info.name) {
                readable(name, info.name, BITEDIT_NAME_WIDTH);
            }
            printf("%u %u %s\n", info.chip, info.field_count, info.name ? name : "-");
            for (uint8_t at = 7; at < 8; at--) {
                printf("%u ", info.bit_field[at]);
            }
            printf("\n");
            for (uint8_t field = 0; field < info.field_count; field++) {
                char text[BITEDIT_BIT_WIDTH + 1] = {0};
                readable(text, info.field_name[field], BITEDIT_BIT_WIDTH);
                printf("%s ", text);
            }
            printf("\n");
        } else if (sscanf(line, "text %x %d", &a, &bit) == 2) {
            RegisterInfo info;
            uint16_t scan = 0;
            char text[256] = {0};
            uint16_t where = BITEDIT_NO_TEXT;
            if (bitedit_lookup((uint16_t)a, &info, &scan)) {
                where = bit < 0 ? info.text
                    : info.bit_field[bit] < info.field_count
                    ? info.field_text[info.bit_field[bit]]
                    : BITEDIT_NO_TEXT;
            }
            readable(text, where, (uint8_t)(sizeof(text) - 1));
            printf("%s\n", text[0] ? text : "-");
        } else if (sscanf(line, "row %x %x %u", &a, &b, &c) == 3) {
            RegisterInfo info;
            uint16_t scan = 0;
            bitedit_lookup(bitedit_io_address(a), &info, &scan);
            uint8_t chip = bitedit_render(row, a, (uint8_t)b, &info, (uint8_t)c);
            printf("%u ", chip);
            put_codes(row);
        } else if (strncmp(line, "header", 6) == 0) {
            bitedit_header(row);
            put_codes(row);
        } else if (strncmp(line, "walk", 4) == 0) {
            uint16_t offset = record_start;
            while (offset < text_start) {
                printf("%04x %u\n", table_word(offset), table_byte((uint16_t)(offset + 2)));
                offset = (uint16_t)(offset + table_byte((uint16_t)(offset + 2)));
            }
            printf("end %u %u %u %u\n", offset, text_start, file_end, (unsigned)database_bytes);
        } else if (sscanf(line, "wrap %u", &a) == 1) {
            /* Exactly one space after the number, so a body that begins or ends
             * with a space survives the parse. */
            char* body = line + 5;
            while (*body >= '0' && *body <= '9') {
                body++;
            }
            body += (*body == ' ');
            uint16_t length = (uint16_t)strcspn(body, "\n");
            for (uint16_t at = 0; at < length;) {
                uint8_t take = bitedit_wrap(body, length, at, (uint8_t)a);
                /* Prefixed, so a wrapped row that is itself "." cannot be read
                 * as the terminator below. */
                printf("=%.*s\n", (int)take, &body[at]);
                if (!take) {
                    break; /* would not terminate; the checker sees the short row */
                }
                at = (uint16_t)(at + take);
            }
            printf(".\n");
        } else if (sscanf(line, "cursor %u", &a) == 1) {
            printf("%u %u\n", bitedit_cursor_left((uint8_t)a), bitedit_cursor_right((uint8_t)a));
        } else if (sscanf(line, "toggle %x %u", &a, &b) == 2) {
            printf("%02x\n", bitedit_toggle((uint8_t)a, (uint8_t)b));
        } else if (sscanf(line, "hex %x %x", &a, &b) == 2) {
            printf("%02x\n", bitedit_hex_digit((uint8_t)a, (uint8_t)b));
        }
        fflush(stdout);
    }
    return 0;
}
