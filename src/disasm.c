/* 45GS02 disassembler for the freeze monitor.
 *
 * The 256-entry opcode map is generated from llvm-mos.  What is hand-written
 * here is the prefix handling, because the 45GS02's three prefixes are not
 * describable as table entries -- and because two of them are absent from every
 * published table, llvm-mos included.  See the notes at each decode site.
 */

#include "disasm.h"

#include "disasm_tables.inc"

static_assert(DISASM_MNEMONIC_WIDTH == 3, "mnemonic field width baked into the layout");
static_assert(sizeof(MODE_LENGTH) == BitZeroPageRelative + 1, "MODE_LENGTH must cover every mode");

/* Opcodes that act as, or are reached through, a prefix. */
constexpr uint8_t OPCODE_NEG = 0x42;
constexpr uint8_t OPCODE_EOM = 0xEA;
constexpr uint8_t OPCODE_CLD = 0xD8;
constexpr uint8_t OPCODE_LDA_INDIRECT_Z = 0xB2;

/* Line layout.  The byte field is padded to five entries rather than the seven
 * a far JSR needs: seven would leave six blank columns on every ordinary
 * instruction, and nothing in the documented instruction set exceeds five.  A
 * far call simply overflows and pushes its own mnemonic right, which is why the
 * caller is told the columns instead of assuming them. */
constexpr uint8_t ADDRESS_DIGITS = 7;
constexpr uint8_t BYTE_FIELD_SLOTS = 5;
constexpr uint8_t MAX_INSTRUCTION_BYTES = 7;
static_assert(DISASM_BYTE_COLUMN == 1 + ADDRESS_DIGITS + 1, "byte column follows the address");
/* Three characters plus at most one suffix -- a Q, an F, or a bit digit -- so
 * the pad below is never zero and the operand column is always derivable. */
static_assert(DISASM_MNEMONIC_FIELD_WIDTH > DISASM_MNEMONIC_WIDTH + 1, "mnemonic field pads");

static const char HEX_DIGITS[] = "0123456789ABCDEF";

/* The five Q mnemonics that are not the base name with a Q appended, stored as
 * base-then-replacement so one table serves both the test and the substitution.
 * Flat rather than [5][6] so it stays an ordinary NUL-terminated literal, which
 * clang requires: a NUL-less string initialiser is an error. */
constexpr uint8_t QUAD_IRREGULAR_COUNT = 5;
constexpr uint8_t QUAD_IRREGULAR_STRIDE = 2 * DISASM_MNEMONIC_WIDTH;
static const char QUAD_IRREGULAR[] = "ORAORQ"
                                     "INCINQ"
                                     "DECDEQ"
                                     "STASTQ"
                                     "LDALDQ";

static char* put_hex(char* cursor, uint32_t value, uint8_t digits) {
    while (digits-- > 0) {
        *cursor++ = HEX_DIGITS[(value >> (digits * 4)) & 0xf];
    }
    return cursor;
}

static uint16_t read_word(const uint8_t* bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

/* Only a far JMP/JSR carries an operand wider than a word, so every other mode
 * reads a fixed two bytes -- a variable-width loop costs far more than the one
 * branch that picks between these. */
static uint32_t read_long(const uint8_t* bytes) {
    return (uint32_t)read_word(bytes) | ((uint32_t)read_word(&bytes[2]) << 16);
}

/* The eight ($nn),Z opcodes -- $12, $32 ... $F2 -- which are the only ones an
 * $EA prefix widens to [$nn],Z. */
static bool is_indirect_zp_z(uint8_t opcode) {
    return (opcode & 0x1f) == 0x12;
}

static bool has_quad_form(uint8_t opcode) {
    return (QUAD_CAPABLE[opcode >> 3] & (1u << (opcode & 7))) != 0;
}

/* The opcodes across which the core preserves the $D8 $D8 far-addressing flag.
 * Taken from the case list at gs4510.vhdl:6912, deliberately not from the
 * comment above it: the comment also names JMP ($nnnn,X) ($7C), but the code
 * does not list it, so on real silicon the prefix does not survive to $7C. */
static bool has_far_form(uint8_t opcode) {
    switch (opcode) {
        case 0x20: /* JSR $nnnn   */
        case 0x22: /* JSR ($nnnn) */
        case 0x23: /* JSR ($nnnn,X) */
        case 0x4C: /* JMP $nnnn   */
        case 0x60: /* RTS         */
        case 0x62: /* RTS #$nn    */
        case 0x6C: /* JMP ($nnnn) */
            return true;
        default:
            return false;
    }
}

/* Branches wrap within the 64K bank, so keep the bank bits of the current
 * address and let the low 16 bits carry.  The result is a full 28-bit address
 * the user can type straight back into D or M. */
static uint32_t branch_target(uint32_t address, uint8_t length, int16_t offset) {
    uint16_t within_bank = (uint16_t)((uint16_t)address + length + (uint16_t)offset);
    return (address & 0x0FFF0000UL) | within_bank;
}

/* Extend bytes[] to cover index, returning how far it now reaches -- which is
 * short of index+1 only where frozen memory runs out.  The count never goes
 * backwards, so a failed prefix probe keeps whatever it did read.  That reuse
 * is the point: the prefix scan looks at the same leading bytes the operand
 * fetch would otherwise read again, about a third of a screenful's reads. */
static uint8_t fetch_through(uint32_t address, uint8_t* bytes, uint8_t fetched, uint8_t index) {
    while (fetched <= index) {
        if (!disasm_read_byte(address + fetched, &bytes[fetched])) {
            break;
        }
        fetched++;
    }
    return fetched;
}

bool disassemble_instruction(uint32_t address, char* text, DisassemblyLayout* layout) {
    uint8_t bytes[MAX_INSTRUCTION_BYTES];
    uint8_t fetched = 0;
    uint8_t prefix_length = 0;
    bool is_quad = false;
    bool is_flat_address = false;
    bool is_far = false;

    fetched = fetch_through(address, bytes, fetched, 0);
    if (fetched == 0) {
        return false;
    }
    uint8_t opcode = bytes[0];

    /* $D8 $D8 widens JMP/JSR/RTS to the 28-bit address space.  This is the one
     * prefix that changes instruction length -- a far JSR carries a 32-bit
     * target -- so failing to decode it would desynchronise every following
     * line by two bytes.  It is live on stock hardware: flat32_enabled defaults
     * to '1' and only the hypervisor can clear it, via $D67D.1. */
    if (opcode == OPCODE_CLD) {
        fetched = fetch_through(address, bytes, fetched, 2);
        if (fetched > 2 && bytes[1] == OPCODE_CLD && has_far_form(bytes[2])) {
            is_far = true;
            prefix_length = 2;
            opcode = bytes[2];
        }
    }
    /* $42 $42 makes the next instruction operate on Q, and may itself be
     * followed by $EA for a 32-bit vector.  Treated as a prefix only when the
     * opcode it reaches actually has a Q form; otherwise these are two ordinary
     * one-byte NEGs, which the core really does execute (it negates A on each
     * before setting the prefix flag), so resuming at the second $42 stays in
     * step with the CPU.  The 39 reserved Q slots land here and render as
     * NEG / NEG / base op -- a mnemonic difference, never an alignment one. */
    else if (opcode == OPCODE_NEG) {
        fetched = fetch_through(address, bytes, fetched, 2);
        if (fetched > 2 && bytes[1] == OPCODE_NEG) {
            uint8_t quad_opcode = bytes[2];
            uint8_t quad_prefix_length = 2;
            if (quad_opcode == OPCODE_EOM) {
                fetched = fetch_through(address, bytes, fetched, 3);
                if (fetched > 3 && is_indirect_zp_z(bytes[3])) {
                    quad_opcode = bytes[3];
                    quad_prefix_length = 3;
                }
            }
            if (has_quad_form(quad_opcode)) {
                is_quad = true;
                is_flat_address = (quad_prefix_length == 3);
                prefix_length = quad_prefix_length;
                opcode = quad_opcode;
            }
        }
    }
    /* $EA before a ($nn),Z opcode widens the vector to 28 bits.  Everywhere
     * else it is EOM. */
    else if (opcode == OPCODE_EOM) {
        fetched = fetch_through(address, bytes, fetched, 1);
        if (fetched > 1 && is_indirect_zp_z(bytes[1])) {
            is_flat_address = true;
            prefix_length = 1;
            opcode = bytes[1];
        }
    }

    const uint8_t mode = OPCODE_MODE[opcode];
    const uint8_t mnemonic_index = OPCODE_MNEMONIC[opcode];

    uint8_t operand_length = MODE_LENGTH[mode] - 1;
    if (is_far && mode == Absolute) {
        operand_length = 4; /* far JMP/JSR carry a 32-bit target */
    }

    const uint8_t length = prefix_length + 1 + operand_length;
    fetched = fetch_through(address, bytes, fetched, length - 1);
    if (fetched < length) {
        return false;
    }
    const uint8_t* operand = &bytes[prefix_length + 1];

    char* cursor = text;
    *cursor++ = ',';
    cursor = put_hex(cursor, address, ADDRESS_DIGITS);
    *cursor++ = ' ';

    for (uint8_t i = 0; i < BYTE_FIELD_SLOTS || i < length; i++) {
        if (i < length) {
            cursor = put_hex(cursor, bytes[i], 2);
        } else {
            *cursor++ = ' ';
            *cursor++ = ' ';
        }
        *cursor++ = ' ';
    }

    layout->mnemonic_column = (uint8_t)(cursor - text);

    const char* name = MNEMONIC_TEXT[mnemonic_index];
    char suffix = 0;
    if (is_quad) {
        suffix = 'Q';
        for (uint8_t i = 0; i < QUAD_IRREGULAR_COUNT; i++) {
            const char* entry = &QUAD_IRREGULAR[i * QUAD_IRREGULAR_STRIDE];
            if (name[0] == entry[0] && name[1] == entry[1] && name[2] == entry[2]) {
                name = &entry[DISASM_MNEMONIC_WIDTH];
                suffix = 0;
                break;
            }
        }
    } else if (is_far) {
        /* These encodings have no published mnemonic.  JSRF/JMPF/RTSF is our
         * spelling, after the core's own "Far-JSR/JMP" and "Far-RTS" comments. */
        suffix = 'F';
    }

    for (uint8_t i = 0; i < DISASM_MNEMONIC_WIDTH; i++) {
        *cursor++ = name[i];
    }
    if (suffix != 0) {
        *cursor++ = suffix;
    } else if (mode == BitZeroPage || mode == BitZeroPageRelative) {
        *cursor++ = (char)('0' + ((opcode >> 4) & 7)); /* RMBn/SMBn/BBRn/BBSn */
    }

    while ((uint8_t)(cursor - text) < layout->mnemonic_column + DISASM_MNEMONIC_FIELD_WIDTH) {
        *cursor++ = ' ';
    }

    /* Q is A, X, Y and Z as one register, so anything but a load would need Z
     * as both index and data.  LDQ is therefore the only Q form that keeps ,Z. */
    const bool index_with_z = !is_quad || opcode == OPCODE_LDA_INDIRECT_Z;

    switch (mode) {
        case Implied:
            break;
        case Accumulator:
            /* Q *is* the accumulator here, so ASLQ takes no operand. */
            if (!is_quad) {
                *cursor++ = 'A';
            }
            break;
        case Immediate:
            *cursor++ = '#';
            *cursor++ = '$';
            cursor = put_hex(cursor, operand[0], 2);
            break;
        case ZeroPage:
        case ZeroPageX:
        case ZeroPageY:
        case BitZeroPage:
        case BitZeroPageRelative:
            *cursor++ = '$';
            cursor = put_hex(cursor, operand[0], 2);
            break;
        case IndirectZeroPageX:
        case IndirectZeroPageY:
        case IndirectZeroPageZ:
        case StackRelativeIndirectY:
            *cursor++ = is_flat_address ? '[' : '(';
            *cursor++ = '$';
            cursor = put_hex(cursor, operand[0], 2);
            break;
        case Absolute:
            *cursor++ = '$';
            cursor = put_hex(cursor,
                operand_length == 4 ? read_long(operand) : read_word(operand),
                (uint8_t)(operand_length * 2));
            break;
        case AbsoluteX:
        case AbsoluteY:
            *cursor++ = '$';
            cursor = put_hex(cursor, read_word(operand), 4);
            break;
        case IndirectAbsolute:
        case IndirectAbsoluteX:
            *cursor++ = '(';
            *cursor++ = '$';
            cursor = put_hex(cursor, read_word(operand), 4);
            break;
        case ImmediateWord:
            *cursor++ = '#';
            *cursor++ = '$';
            cursor = put_hex(cursor, read_word(operand), 4);
            break;
        case Relative8:
            *cursor++ = '$';
            cursor =
                put_hex(cursor, branch_target(address, length, (int8_t)operand[0]), ADDRESS_DIGITS);
            break;
        case Relative16:
            *cursor++ = '$';
            cursor = put_hex(cursor,
                branch_target(address, length, (int16_t)read_word(operand)),
                ADDRESS_DIGITS);
            break;
        default:
            break;
    }

    /* Index and closing-bracket suffixes, kept separate from the operand itself
     * so each mode states only what it adds. */
    switch (mode) {
        case ZeroPageX:
        case AbsoluteX:
            *cursor++ = ',';
            *cursor++ = 'X';
            break;
        case ZeroPageY:
        case AbsoluteY:
            *cursor++ = ',';
            *cursor++ = 'Y';
            break;
        case IndirectZeroPageX:
            *cursor++ = ',';
            *cursor++ = 'X';
            *cursor++ = ')';
            break;
        case IndirectZeroPageY:
            /* Not bracket-aware: only the ($nn),Z opcodes take the $EA prefix. */
            *cursor++ = ')';
            *cursor++ = ',';
            *cursor++ = 'Y';
            break;
        case IndirectZeroPageZ:
            *cursor++ = is_flat_address ? ']' : ')';
            if (index_with_z) {
                *cursor++ = ',';
                *cursor++ = 'Z';
            }
            break;
        case StackRelativeIndirectY:
            *cursor++ = ',';
            *cursor++ = 'S';
            *cursor++ = 'P';
            *cursor++ = ')';
            *cursor++ = ',';
            *cursor++ = 'Y';
            break;
        case IndirectAbsolute:
            *cursor++ = ')';
            break;
        case IndirectAbsoluteX:
            *cursor++ = ',';
            *cursor++ = 'X';
            *cursor++ = ')';
            break;
        case BitZeroPageRelative:
            *cursor++ = ',';
            *cursor++ = '$';
            cursor =
                put_hex(cursor, branch_target(address, length, (int8_t)operand[1]), ADDRESS_DIGITS);
            break;
        default:
            break;
    }

    /* Drop the mnemonic field's padding again when nothing followed it, so an
     * operand-less instruction does not carry trailing blanks onto the screen. */
    while (cursor > text && cursor[-1] == ' ') {
        cursor--;
    }
    *cursor = '\0';

    layout->length = length;
    layout->text_length = (uint8_t)(cursor - text);
    layout->mnemonic_class = DisasmClassPlain;
    if (is_quad || is_flat_address || is_far) {
        layout->mnemonic_class = DisasmClassMega65;
    } else if ((CONTROL_FLOW[mnemonic_index >> 3] & (1u << (mnemonic_index & 7))) != 0) {
        layout->mnemonic_class = DisasmClassControlFlow;
    }
    return true;
}
