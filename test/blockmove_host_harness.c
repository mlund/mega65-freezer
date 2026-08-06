/* Host driver for the block operations, used only by test/verify_blockmove.py.
 *
 * blockmove.c takes its two memory accesses from the caller, so on the host
 * they become a flat array and the logic can be exercised without a MEGA65.
 * Reads and writes outside the array fail, which is how the unmapped paths are
 * reached.
 *
 * Commands on stdin, one per line:
 *   pattern            fill memory with a deterministic non-trivial pattern
 *   set <addr> <val>   store one byte
 *   move <from> <to> <len>   -> "ok" or "fail"
 *   read <addr> <len>        -> "<count> <hex bytes>"
 *   dump <addr> <len>        -> hex bytes
 * All numbers are decimal.
 */

#include "blockmove.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Larger than one chunk, so multi-chunk paths are reachable, and small enough
 * that running off the end is easy to arrange. */
#define MEMORY_SIZE 2048
static unsigned char memory[MEMORY_SIZE];

bool disasm_read_byte(uint32_t address, uint8_t* value) {
    if (address >= MEMORY_SIZE) {
        return false;
    }
    *value = memory[address];
    return true;
}

bool block_write_byte(uint32_t address, unsigned char value) {
    if (address >= MEMORY_SIZE) {
        return false;
    }
    memory[address] = value;
    return true;
}

static void print_bytes(uint32_t address, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        printf("%s%02x", i ? " " : "", address + i < MEMORY_SIZE ? memory[address + i] : 0);
    }
    printf("\n");
}

int main(void) {
    char line[128];

    while (fgets(line, sizeof(line), stdin)) {
        char command[16];
        unsigned long a = 0, b = 0, c = 0;
        int fields = sscanf(line, "%15s %lu %lu %lu", command, &a, &b, &c);
        if (fields < 1) {
            continue;
        }

        if (strcmp(command, "pattern") == 0) {
            /* Not i & 0xff: a byte that repeats every 256 would hide an
             * off-by-one that lands a chunk boundary away. */
            for (uint32_t i = 0; i < MEMORY_SIZE; i++) {
                memory[i] = (unsigned char)((i * 7u + 3u) ^ (i >> 5));
            }
        } else if (strcmp(command, "set") == 0) {
            if (a < MEMORY_SIZE) {
                memory[a] = (unsigned char)b;
            }
        } else if (strcmp(command, "move") == 0) {
            printf("%s\n", block_move((uint32_t)a, (uint32_t)b, (uint32_t)c) ? "ok" : "fail");
        } else if (strcmp(command, "read") == 0) {
            uint16_t got = block_read((uint32_t)a, (uint32_t)b);
            printf("%u", got);
            for (uint16_t i = 0; i < got; i++) {
                printf(" %02x", block_buffer[i]);
            }
            printf("\n");
        } else if (strcmp(command, "dump") == 0) {
            print_bytes((uint32_t)a, (uint32_t)b);
        }
        fflush(stdout);
    }
    return 0;
}
