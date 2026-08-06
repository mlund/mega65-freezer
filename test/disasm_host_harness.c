/* Host driver for the 45GS02 disassembler, used only by test/verify_disasm.py.
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

/* One instruction per line, at a fixed base address unless the line opens with
 * "@<hex>" -- which branch operands need, since they name a target rather than
 * an offset and so depend on where the instruction sits. */
static int decode_lines(void) {
    static uint8_t buffer[16];
    char line[256];

    image = buffer;

    while (fgets(line, sizeof line, stdin) != NULL) {
        image_length = 0;
        image_base = 0x0001000UL;
        memset(buffer, 0, sizeof buffer);

        for (char* token = strtok(line, " \t\n"); token != NULL; token = strtok(NULL, " \t\n")) {
            unsigned value;
            if (token[0] == '@') {
                image_base = strtoul(&token[1], NULL, 16);
                continue;
            }
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

/* One instruction per line as "<address hex> <mnemonic> [operand]", answering
 * with the assembled bytes or the status that refused them. */
static int assemble_lines(void) {
    char line[256];

    while (fgets(line, sizeof line, stdin) != NULL) {
        char* newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        char* rest = strchr(line, ' ');
        if (rest == NULL) {
            continue;
        }
        *rest++ = '\0';

        uint8_t bytes[ASM_BYTES_MAX];
        uint8_t length = 0;
        enum AssembleStatus status =
            assemble_instruction((uint32_t)strtoul(line, NULL, 16), rest, bytes, &length);
        if (status != AssembleOk) {
            printf("!%u\n", (unsigned)status);
            continue;
        }
        for (uint8_t i = 0; i < length; i++) {
            printf("%s%02X", i ? " " : "", bytes[i]);
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 6 && strcmp(argv[1], "--walk") == 0) {
        return walk(argv[2], (uint32_t)strtoul(argv[3], NULL, 16),
            (uint32_t)strtoul(argv[4], NULL, 16), strtol(argv[5], NULL, 10));
    }
    if (argc == 2 && strcmp(argv[1], "--assemble") == 0) {
        return assemble_lines();
    }
    return decode_lines();
}
