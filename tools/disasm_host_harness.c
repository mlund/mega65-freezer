/* Host driver for the 45GS02 disassembler, used only by tools/verify_disasm.py.
 *
 * Compiling the same disasm.c for the host is what lets the test diff real
 * decoder output against llvm-mc, rather than testing a Python reimplementation
 * that could drift from the C.
 *
 * Reads one instruction per line as space-separated hex bytes on stdin, writes
 * "<length>|<text>" per line on stdout.
 */

#include "disasm.h"

#include <stdio.h>
#include <string.h>

/* The instruction under test is loaded here, at a fixed base address. */
#define HARNESS_BASE 0x0001000UL
static uint8_t harness_memory[16];
static uint8_t harness_length;

bool disasm_read_byte(uint32_t address, uint8_t* value) {
    uint32_t offset = address - HARNESS_BASE;
    if (offset >= harness_length) {
        return false;
    }
    *value = harness_memory[offset];
    return true;
}

int main(void) {
    char line[256];

    while (fgets(line, sizeof line, stdin) != NULL) {
        harness_length = 0;
        memset(harness_memory, 0, sizeof harness_memory);

        for (char* token = strtok(line, " \t\n"); token != NULL; token = strtok(NULL, " \t\n")) {
            unsigned value;
            if (sscanf(token, "%x", &value) != 1 || harness_length >= sizeof harness_memory) {
                break;
            }
            harness_memory[harness_length++] = (uint8_t)value;
        }
        if (harness_length == 0) {
            continue;
        }

        char text[DISASM_TEXT_MAX];
        DisassemblyLayout layout;
        if (disassemble_instruction(HARNESS_BASE, text, &layout)) {
            printf("%u|%s\n", layout.length, text);
        } else {
            printf("0|<truncated>\n");
        }
    }
    return 0;
}
