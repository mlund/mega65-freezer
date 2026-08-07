/* Host driver for the number formatting, used only by test/verify_format.py.
 *
 * src/format.c writes into buffers the caller owns and touches no hardware, so
 * the host can run it and check it against Python's own formatting.  That is
 * the whole reason the pure part was separated from screen.c.
 *
 * Commands on stdin, one per line.  All numbers are decimal:
 *   hex <value> <columns>   -> the digits, as screen codes rendered to ASCII
 *   dec <value>             -> five columns, space-padded
 *   nybl <value>            -> one digit
 *   appenddec <value>       -> the digits, no padding
 *   appenddec2 <value>      -> exactly two digits
 *   appendhex <value> <columns>
 */

#include "../src/format.c"

#include <stdio.h>
#include <string.h>

/* format_hex emits screen codes, where 'A'-'F' are 1-6 rather than ASCII.
 * Rendering them back makes the expected values readable in the checker. */
static char from_screen_code(char c) {
    unsigned char v = (unsigned char)c;
    if (v >= 0x30 && v <= 0x39) {
        return (char)v;
    }
    if (v >= 1 && v <= 6) {
        return (char)('A' + v - 1);
    }
    return '?';
}

int main(void) {
    char line[128];

    while (fgets(line, sizeof line, stdin)) {
        char cmd[32];
        long a = 0;
        int b = 0;
        char out[64];

        memset(out, 0, sizeof out);
        const int n = sscanf(line, "%31s %ld %d", cmd, &a, &b);
        if (n < 1) {
            continue;
        }

        if (!strcmp(cmd, "hex")) {
            format_hex(out, a, (char)b);
            for (int i = 0; i < b; i++) {
                out[i] = from_screen_code(out[i]);
            }
            out[b] = '\0';
        } else if (!strcmp(cmd, "dec")) {
            format_decimal(out, (uint16_t)a);
            out[DECIMAL_COLUMNS] = '\0';
        } else if (!strcmp(cmd, "nybl")) {
            out[0] = from_screen_code((char)nybl_to_screen((uint8_t)a));
            out[1] = '\0';
        } else if (!strcmp(cmd, "appenddec")) {
            *append_dec(out, (uint16_t)a) = '\0';
        } else if (!strcmp(cmd, "appenddec2")) {
            *append_dec2(out, (uint8_t)a) = '\0';
        } else if (!strcmp(cmd, "appendhex")) {
            char* end = append_hex(out, a, (uint8_t)b);
            for (char* p = out; p < end; p++) {
                *p = from_screen_code(*p);
            }
            *end = '\0';
        } else {
            printf("?\n");
            fflush(stdout);
            continue;
        }

        printf("%s\n", out);
        fflush(stdout);
    }
    return 0;
}
