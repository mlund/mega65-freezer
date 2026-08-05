#pragma once

/* 45GS02 disassembler for the freeze monitor's `D` command.
 *
 * The opcode tables come from llvm-mos (see tools/gen_disasm_tables.py); the
 * three instruction prefixes are decoded here.
 */

#include <stdbool.h>
#include <stdint.h>

/* Longest line the formatter can produce: a far JSR pushes the mnemonic out to
 * column 30, and BBRn's `$nn,$nnnnnnn` is the widest operand. */
constexpr uint8_t DISASM_TEXT_MAX = 48;

/* Field geometry, so a caller can colour the line without re-deriving it.  Only
 * the mnemonic column varies, because an instruction longer than five bytes
 * overflows the byte field and pushes everything after it right. */
constexpr uint8_t DISASM_BYTE_COLUMN = 9;
constexpr uint8_t DISASM_MNEMONIC_FIELD_WIDTH = 5;

/* What the mnemonic does, for callers that want to distinguish it visually.
 * Deliberately not a colour: the decoder has no business knowing the palette. */
enum DisasmMnemonicClass : uint8_t {
    DisasmClassPlain,
    DisasmClassControlFlow, /* branches, jumps, subroutine calls and returns */
    DisasmClassMega65,      /* Q, 28-bit flat, or far: the 45GS02-only forms */
};

typedef struct {
    uint8_t length;          /* instruction length in bytes, prefixes included */
    uint8_t mnemonic_column; /* operand follows at + DISASM_MNEMONIC_FIELD_WIDTH */
    uint8_t text_length;     /* total characters written to `text` */
    uint8_t mnemonic_class;
} DisassemblyLayout;

/* Supplied by the caller: read one byte of frozen memory.  Returns false if the
 * address is not present in the freeze slot, which aborts the line. */
extern bool disasm_read_byte(uint32_t address, uint8_t* value);

/* Format the instruction at `address` into `text` (at least DISASM_TEXT_MAX
 * bytes) and describe it in `layout`.  Returns false if any byte of the
 * instruction could not be read, in which case `text` is untouched. */
bool disassemble_instruction(uint32_t address, char* text, DisassemblyLayout* layout);
