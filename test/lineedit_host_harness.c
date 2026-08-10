/* Drives line_edit() on the host: keys in on stdin, the buffer out on stdout.
 *
 * The editor's own copy is compiled in, not a reimplementation, so what the
 * tests exercise is what the tool runs.  Same shape as layout_host_harness.c
 * and slotmap_host_harness.c. */

#include "../src/sprited/lineedit.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <capacity> [key ...]\n", argv[0]);
        return 2;
    }

    const uint8_t capacity = (uint8_t)atoi(argv[1]);
    char buffer[256];
    uint8_t length = 0;
    bool done = false;

    memset(buffer, 0xAA, sizeof buffer); /* so a missing terminator shows */
    buffer[0] = '\0';

    for (int i = 2; i < argc && !done; i++) {
        done = line_edit(buffer, capacity, &length, (uint8_t)atoi(argv[i]));
    }

    /* length, whether RETURN ended it, and the bytes -- printed as numbers so
     * a stray control character cannot be mistaken for the terminator. */
    printf("%u %d", length, done ? 1 : 0);
    /* int, not uint8_t: a broken editor can leave length at 255, and the
     * comparison would then never end. */
    for (int i = 0; i <= (int)length; i++) {
        printf(" %u", (unsigned char)buffer[i]);
    }
    printf("\n");
    return 0;
}
