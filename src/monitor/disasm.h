#pragma once

/* 45GS02 disassembler for the freeze monitor's `D` command.
 *
 * The opcode tables come from llvm-mos (see tools/gen_disasm_tables.py); the
 * three instruction prefixes are decoded here.
 *
 * Host-compilable: no MEGA65 dependency, and test/verify_disasm.py builds it
 * for the host to diff against llvm-mc.  disasm_read_byte() is the whole
 * platform interface.
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

/* Scan hex digits from `text`, returning where they stopped.  `digits` reports
 * how many were read, which is what tells $12 from $0012. */
const char* disasm_parse_hex(const char* text, uint32_t* value, uint8_t* digits);

/* Why an assemble failed, so the caller can say which part it disliked. */
enum AssembleStatus : uint8_t {
    AssembleOk,
    AssembleUnknownMnemonic,
    AssembleBadOperand,
    AssembleWrongOperand, /* the mnemonic has no form taking this operand */
    AssembleBranchTooFar,
};

/* Longest 45GS02 instruction: a far JSR.  Bounds both directions. */
constexpr uint8_t ASM_BYTES_MAX = 7;

/* Assemble one instruction written as `MNEMONIC [operand]` into `bytes`, with
 * `address` supplying the origin that branch operands are measured from.
 * `length` receives the byte count on success and is untouched otherwise. */
enum AssembleStatus assemble_instruction(
    uint32_t address, const char* text, uint8_t* bytes, uint8_t* length);
