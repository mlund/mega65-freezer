/* Host driver for the 45GS02 disassembler, used only by tools/verify_disasm.py.
 *
 * Compiling the same disasm.c for the host is what lets the test diff real
 * decoder output against llvm-mc, rather than testing a Python reimplementation
 * that could drift from the C.
 *
 * Two modes:
 *
 *   (no arguments)                       one instruction per stdin line, given
 *                                        as space-separated hex bytes
 *   --walk <file> <base> <start> <count> successive instructions from a binary
 *                                        image loaded at <base>
 *
 * Both write "<length> <text>" per line on stdout.
 */

#include "disasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Whatever is being decoded, and the address its first byte occupies. */
static uint8_t* image;
static long image_length;
static uint32_t image_base;

bool disasm_read_byte(uint32_t address, uint8_t* value) {
    long offset = (long)address - (long)image_base;
    if (offset < 0 || offset >= image_length) {
        return false;
    }
    *value = image[offset];
    return true;
}

static void emit(uint32_t address) {
    char text[DISASM_TEXT_MAX];
    DisassemblyLayout layout;

    if (disassemble_instruction(address, text, &layout)) {
        printf("%u %s\n", layout.length, text);
    } else {
        printf("0 <truncated>\n");
    }
}

/* One instruction per line, at a fixed base address. */
static int decode_lines(void) {
    static uint8_t buffer[16];
    char line[256];

    image = buffer;
    image_base = 0x0001000UL;

    while (fgets(line, sizeof line, stdin) != NULL) {
        image_length = 0;
        memset(buffer, 0, sizeof buffer);

        for (char* token = strtok(line, " \t\n"); token != NULL; token = strtok(NULL, " \t\n")) {
            unsigned value;
            if (sscanf(token, "%x", &value) != 1 || image_length >= (long)sizeof buffer) {
                break;
            }
            buffer[image_length++] = (uint8_t)value;
        }
        if (image_length > 0) {
            emit(image_base);
        }
    }
    return 0;
}

/* Successive instructions through a binary image, following the lengths the
 * decoder reports -- which is what exercises the boundaries that a listing
 * desynchronises on. */
static int walk(const char* path, uint32_t base, uint32_t start, long count) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseek(file, 0, SEEK_END);
    image_length = ftell(file);
    fseek(file, 0, SEEK_SET);

    image = malloc((size_t)image_length);
    if (image == NULL || fread(image, 1, (size_t)image_length, file) != (size_t)image_length) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    fclose(file);
    image_base = base;

    char text[DISASM_TEXT_MAX];
    DisassemblyLayout layout;
    for (long i = 0; i < count; i++) {
        if (!disassemble_instruction(start, text, &layout)) {
            break;
        }
        printf("%u %s\n", layout.length, text);
        start += layout.length;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 6 && strcmp(argv[1], "--walk") == 0) {
        return walk(argv[2], (uint32_t)strtoul(argv[3], NULL, 16),
            (uint32_t)strtoul(argv[4], NULL, 16), strtol(argv[5], NULL, 10));
    }
    return decode_lines();
}
