/* Host driver for the bit editor's table, used only by test/verify_bitedit.py.
 *
 * bitedit_table.c reads a static table and writes into the caller's buffer, so
 * on the host it needs nothing but a compiler.  The source is included rather
 * than linked: `walk` has to step the raw record stream to prove the length
 * byte is right, and that stream is private to the translation unit.
 *
 * Commands on stdin, one per line.  Addresses and values are hex, everything
 * else decimal, and rendered rows come back as hex screen codes so the checker
 * sees exactly what would reach the screen:
 *   lookup <addr>              -> "<chip> <fields> <name|->", 8 bit indices,
 *                                 then one line of space-separated field names
 *   cursor <cell>              -> "<left> <right>", one step each way
 *   row <addr> <val> <prev>    -> "<chip> <hex bytes>"
 *   header                     -> hex bytes
 *   walk                       -> "<addr> <length>" per record
 *   toggle <val> <bit>         -> the new value
 *   hex <val> <digit>          -> the new value
 */

#include "bitedit_table.c"

#include <stdio.h>
#include <string.h>

/* One stored name back to ASCII, trailing spaces trimmed, so the checker can
 * compare it with what the generator's own abbreviation produced. */
static void readable(char* out, const uint8_t* name, uint8_t limit) {
    uint8_t length = copy_name(out, name, limit);
    for (uint8_t i = 0; i < length; i++) {
        unsigned char code = (unsigned char)out[i];
        out[i] = code >= 1 && code <= 26 ? (char)(code + 0x40) : (char)code;
    }
    out[length] = '\0';
}

static void put_codes(const char* row) {
    for (uint8_t i = 0; i < BITEDIT_ROW_WIDTH; i++) {
        printf("%02x", (unsigned char)row[i]);
    }
    printf("\n");
}

int main(void) {
    char line[128];
    char row[BITEDIT_ROW_WIDTH];

    while (fgets(line, sizeof(line), stdin)) {
        unsigned a, b, c;

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
            for (uint8_t bit = 7; bit < 8; bit--) {
                printf("%u ", info.bit_field[bit]);
            }
            printf("\n");
            for (uint8_t field = 0; field < info.field_count; field++) {
                char text[BITEDIT_BIT_WIDTH + 1] = {0};
                readable(text, info.field[field], BITEDIT_BIT_WIDTH);
                printf("%s ", text);
            }
            printf("\n");
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
            uint16_t offset = 0;
            while (offset < IOMAP_TABLE_BYTES) {
                const uint8_t* record = &IOMAP_TABLE[offset];
                printf("%04x %u\n", record[0] | (record[1] << 8), record[2]);
                offset = (uint16_t)(offset + record[2]);
            }
            printf("end %u %u\n", offset, (unsigned)IOMAP_TABLE_BYTES);
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
