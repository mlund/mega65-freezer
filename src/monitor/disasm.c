/* 45GS02 disassembler and assembler for the freeze monitor.
 *
 * The opcode map is generated from llvm-mos; the three prefixes are hand-written
 * here, two of them being absent from every published table.
 *
 * One translation unit for both directions: a separate assembler measured 66
 * bytes larger.  Not because the tables would be duplicated -- LTO merges
 * identical statics, so including them twice costs nothing.
 *
 * Host-compilable -- see disasm.h.
 */

#include "disasm.h"

#include "disasm_tables.inc"

/* Keeps a table out of zero page.  `const` does not: the LTO allocator puts
 * const data there, and every byte it takes is one the imaginary registers do
 * not get, which costs far more in code than the table's own size -- these
 * three are worth 325 bytes of MONITOR.  Guarded because this file is also
 * built for the host by test/verify_disasm.py, where section names are the
 * host's ("segment,section" on Mach-O). */
#if defined(__mos__)
#define RODATA __attribute__((section(".rodata")))
#else
#define RODATA
#endif

static_assert(DISASM_MNEMONIC_WIDTH == 3, "mnemonic field width baked into the layout");
static_assert(sizeof(MODE_LENGTH) == BitZeroPageRelative + 1, "MODE_LENGTH must cover every mode");

/* Opcodes that act as, or are reached through, a prefix. */
constexpr uint8_t OPCODE_NEG = 0x42;
constexpr uint8_t OPCODE_EOM = 0xEA;
constexpr uint8_t OPCODE_CLD = 0xD8;
constexpr uint8_t OPCODE_LDA_INDIRECT_Z = 0xB2;

/* Five byte slots, not the seven a far JSR needs: nothing documented exceeds
 * five, and a far call may overflow and push its own mnemonic right -- which is
 * why the caller is told the columns rather than assuming them. */
constexpr uint8_t ADDRESS_DIGITS = 7;
constexpr uint8_t BYTE_FIELD_SLOTS = 5;
static_assert(DISASM_BYTE_COLUMN == 1 + ADDRESS_DIGITS + 1, "byte column follows the address");
/* Three characters plus at most one suffix -- a Q, an F, or a bit digit -- so
 * the pad below is never zero and the operand column is always derivable. */
static_assert(DISASM_MNEMONIC_FIELD_WIDTH > DISASM_MNEMONIC_WIDTH + 1, "mnemonic field pads");

static const char HEX_DIGITS[] RODATA = "0123456789ABCDEF";

/* The Q mnemonics that are not the base name plus a Q, base then replacement.
 * Flat because clang rejects a NUL-less string initialiser. */
constexpr uint8_t QUAD_IRREGULAR_COUNT = 5;
constexpr uint8_t QUAD_IRREGULAR_STRIDE = 2 * DISASM_MNEMONIC_WIDTH;
static const char QUAD_IRREGULAR[] RODATA = "ORAORQ"
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

/* Only a far JMP/JSR exceeds a word, and a variable-width loop cost far more
 * than the one branch that picks between these two. */
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
static const uint8_t FAR_OPCODES[] RODATA = {
    0x20, /* JSR $nnnn     */
    0x22, /* JSR ($nnnn)   */
    0x23, /* JSR ($nnnn,X) */
    0x4C, /* JMP $nnnn     */
    0x60, /* RTS           */
    0x62, /* RTS #$nn      */
    0x6C, /* JMP ($nnnn)   */
};

static bool has_far_form(uint8_t opcode) {
    for (uint8_t i = 0; i < (uint8_t)sizeof FAR_OPCODES; i++) {
        if (FAR_OPCODES[i] == opcode) {
            return true;
        }
    }
    return false;
}

/* Branches wrap within the 64K bank, so keep the bank bits of the current
 * address and let the low 16 bits carry.  The result is a full 28-bit address
 * the user can type straight back into D or M. */
static uint32_t branch_target(uint32_t address, uint8_t length, int16_t offset) {
    uint16_t within_bank = (uint16_t)((uint16_t)address + length + (uint16_t)offset);
    return (address & 0x0FFF0000UL) | within_bank;
}

/* Extend bytes[] to cover index, returning how far it reaches.  Never goes
 * backwards, so a failed prefix probe keeps what it read -- worth about a third
 * of a screenful's frozen-memory reads. */
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
    uint8_t bytes[ASM_BYTES_MAX];
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

    /* $D8 $D8 widens JMP/JSR/RTS to 28 bits, and is the one prefix that changes
     * instruction length -- missing it desynchronises every following line.
     * Live on stock hardware: flat32_enabled defaults to '1'. */
    if (opcode == OPCODE_CLD) {
        fetched = fetch_through(address, bytes, fetched, 2);
        if (fetched > 2 && bytes[1] == OPCODE_CLD && has_far_form(bytes[2])) {
            is_far = true;
            prefix_length = 2;
            opcode = bytes[2];
        }
    }
    /* $42 $42 makes the next instruction operate on Q, optionally through $EA.
     * A prefix only where the opcode has a Q form: otherwise these are two real
     * NEGs the core executes, so resuming at the second stays in step. */
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

/* --- Assembler -------------------------------------------------------------
 *
 * No reverse table: (mnemonic, mode) already identifies an opcode in the maps
 * above, uniquely but for the bit-numbered families where the digit decides.
 * Scanning 256 entries is free when a human just typed the line.
 * ------------------------------------------------------------------------ */

/* The operand shapes the syntax admits, before deciding how wide the value is. */
enum OperandShape : uint8_t {
    ShapeNone,        /* (nothing)      */
    ShapeAccumulator, /* A              */
    ShapeImmediate,   /* #$v            */
    ShapeValue,       /* $v             */
    ShapeValueX,      /* $v,X           */
    ShapeValueY,      /* $v,Y           */
    ShapeIndirectX,   /* ($v,X)         */
    ShapeIndirectY,   /* ($v),Y         */
    ShapeIndirectZ,   /* ($v),Z         */
    ShapeStackY,      /* ($v,SP),Y      */
    ShapeIndirect,    /* ($v)           */
    ShapeValuePair,   /* $v,$t          */
};

/* Modes each shape can mean, narrowest first so that $12 assembles to the
 * zero-page form and $0012 to the absolute one.  MODE_NONE ends each row. */
constexpr uint8_t MODE_NONE = 0xFF;
constexpr uint8_t SHAPE_MODE_MAX = 5;
static const uint8_t SHAPE_MODES[][SHAPE_MODE_MAX] = {
    [ShapeNone] = {Implied, Accumulator, MODE_NONE},
    [ShapeAccumulator] = {Accumulator, MODE_NONE},
    [ShapeImmediate] = {Immediate, ImmediateWord, MODE_NONE},
    /* RMBn and the branches share this shape with ordinary loads and stores;
     * only the mnemonic tells them apart, so those forms simply come later. */
    [ShapeValue] = {ZeroPage, BitZeroPage, Absolute, Relative8, Relative16},
    [ShapeValueX] = {ZeroPageX, AbsoluteX, MODE_NONE},
    [ShapeValueY] = {ZeroPageY, AbsoluteY, MODE_NONE},
    [ShapeIndirectX] = {IndirectZeroPageX, IndirectAbsoluteX, MODE_NONE},
    [ShapeIndirectY] = {IndirectZeroPageY, MODE_NONE},
    [ShapeIndirectZ] = {IndirectZeroPageZ, MODE_NONE},
    [ShapeStackY] = {StackRelativeIndirectY, MODE_NONE},
    /* A Q form drops the ,Z the decoder prints for LDQ alone, so ($nn) has to
     * reach the base-page indirect as well as the absolute one. */
    [ShapeIndirect] = {IndirectZeroPageZ, IndirectAbsolute, MODE_NONE},
    [ShapeValuePair] = {BitZeroPageRelative, MODE_NONE},
};

/* File-scope rather than threaded through every helper; -fnonreentrant. */
static const char* scan;

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static void skip_spaces(void) {
    while (*scan == ' ') {
        scan++;
    }
}

/* Consume `c` if it is next, spaces first. */
static bool accept(char c) {
    skip_spaces();
    if (upper(*scan) != c) {
        return false;
    }
    scan++;
    return true;
}

const char* disasm_parse_hex(const char* text, uint32_t* value, uint8_t* digits) {
    *value = 0;
    *digits = 0;
    for (;;) {
        char c = upper(*text);
        uint8_t nibble;
        if (c >= '0' && c <= '9') {
            nibble = (uint8_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint8_t)(c - 'A' + 10);
        } else {
            return text;
        }
        *value = (*value << 4) | nibble;
        (*digits)++;
        text++;
    }
}

/* Hex, '$' optional. */
static bool accept_value(uint32_t* value, uint8_t* digits) {
    (void)accept('$');
    scan = disasm_parse_hex(scan, value, digits);
    return *digits > 0;
}

/* The shapes opening with a bracket, the opener already consumed.  `closer` is
 * the matching one, so [$12,X) cannot pass for ($12,X). */
static bool parse_indirect(uint8_t* shape, uint32_t* value, uint8_t* digits, char closer) {
    if (!accept_value(value, digits)) {
        return false;
    }
    if (accept(',')) {
        if (accept('S')) {
            *shape = ShapeStackY;
            return accept('P') && accept(closer) && accept(',') && accept('Y');
        }
        *shape = ShapeIndirectX;
        return accept('X') && accept(closer);
    }
    if (!accept(closer)) {
        return false;
    }
    if (!accept(',')) {
        /* ($nnnn), or [$nn] where a Q form has dropped the ,Z. */
        *shape = ShapeIndirect;
        return true;
    }
    if (accept('Y')) {
        *shape = ShapeIndirectY;
        return true;
    }
    *shape = ShapeIndirectZ;
    return accept('Z');
}

/* Classify what follows the mnemonic.  `flat` reports the [..] spelling, which
 * the caller turns into the $EA prefix. */
static bool parse_operand(
    uint8_t* shape, uint32_t* value, uint8_t* digits, uint32_t* second, bool* flat) {
    *flat = false;
    *second = 0;

    skip_spaces();
    if (*scan == '\0') {
        *shape = ShapeNone;
        return true;
    }
    /* A lone A is the accumulator; anything after it is a hex value, since A is
     * also a hex digit. */
    if (upper(*scan) == 'A' && (scan[1] == '\0' || scan[1] == ' ')) {
        scan++;
        *shape = ShapeAccumulator;
        return true;
    }
    if (accept('#')) {
        *shape = ShapeImmediate;
        return accept_value(value, digits);
    }

    if (accept('(')) {
        return parse_indirect(shape, value, digits, ')');
    }
    if (accept('[')) {
        *flat = true;
        return parse_indirect(shape, value, digits, ']');
    }

    if (!accept_value(value, digits)) {
        return false;
    }
    if (!accept(',')) {
        *shape = ShapeValue;
        return true;
    }
    if (accept('X')) {
        *shape = ShapeValueX;
        return true;
    }
    if (accept('Y')) {
        *shape = ShapeValueY;
        return true;
    }

    /* BBRn $nn,$nnnn -- a second value rather than an index register. */
    uint8_t second_digits;
    if (!accept_value(second, &second_digits)) {
        return false;
    }
    *shape = ShapeValuePair;
    return true;
}

/* The opcode for this mnemonic and mode; `bit` picks among RMBn and friends. */
static bool find_opcode(uint8_t mnemonic_index, uint8_t mode, uint8_t bit, uint8_t* opcode) {
    for (uint16_t candidate = 0; candidate < 256; candidate++) {
        if (OPCODE_MNEMONIC[candidate] != mnemonic_index || OPCODE_MODE[candidate] != mode) {
            continue;
        }
        if ((mode == BitZeroPage || mode == BitZeroPageRelative) && ((candidate >> 4) & 7) != bit) {
            continue;
        }
        *opcode = (uint8_t)candidate;
        return true;
    }
    return false;
}

/* Inverse of branch_target(). */
static bool branch_offset(
    uint32_t address, uint8_t length, uint32_t target, bool wide, uint16_t* offset) {
    uint16_t delta = (uint16_t)(target - ((uint16_t)address + length));
    if (!wide && (int16_t)delta != (int8_t)delta) {
        return false;
    }
    *offset = delta;
    return true;
}

enum AssembleStatus assemble_instruction(
    uint32_t address, const char* text, uint8_t* bytes, uint8_t* length) {
    char name[4] = {0};
    uint8_t name_length = 0;
    uint8_t bit = 0;
    bool want_quad = false;
    bool want_far = false;

    scan = text;
    skip_spaces();
    while (name_length < 4) {
        char c = upper(*scan);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            break;
        }
        name[name_length++] = c;
        scan++;
    }
    if (name_length < 3) {
        return AssembleUnknownMnemonic;
    }

    /* A fourth character is a suffix: RMBn's bit, a Q form, or a far F. */
    if (name_length == 4) {
        char suffix = name[3];
        if (suffix >= '0' && suffix <= '7') {
            bit = (uint8_t)(suffix - '0');
        } else if (suffix == 'Q') {
            want_quad = true;
        } else if (suffix == 'F') {
            want_far = true;
        } else {
            return AssembleUnknownMnemonic;
        }
    }

    /* The Q forms whose names are not the base plus a Q, through the same
     * table the decoder prints them from. */
    for (uint8_t i = 0; !want_quad && i < QUAD_IRREGULAR_COUNT; i++) {
        const char* entry = &QUAD_IRREGULAR[i * QUAD_IRREGULAR_STRIDE];
        if (name[0] == entry[DISASM_MNEMONIC_WIDTH] &&
            name[1] == entry[DISASM_MNEMONIC_WIDTH + 1] &&
            name[2] == entry[DISASM_MNEMONIC_WIDTH + 2]) {
            name[0] = entry[0];
            name[1] = entry[1];
            name[2] = entry[2];
            want_quad = true;
        }
    }

    uint8_t mnemonic_index = 0;
    while (mnemonic_index < DISASM_MNEMONIC_COUNT &&
        (MNEMONIC_TEXT[mnemonic_index][0] != name[0] ||
            MNEMONIC_TEXT[mnemonic_index][1] != name[1] ||
            MNEMONIC_TEXT[mnemonic_index][2] != name[2])) {
        mnemonic_index++;
    }
    if (mnemonic_index == DISASM_MNEMONIC_COUNT) {
        return AssembleUnknownMnemonic;
    }

    uint8_t shape;
    uint32_t value = 0;
    uint32_t second = 0;
    uint8_t digits = 0;
    bool flat = false;
    if (!parse_operand(&shape, &value, &digits, &second, &flat)) {
        return AssembleBadOperand;
    }
    skip_spaces();
    if (*scan != '\0') {
        return AssembleBadOperand;
    }

    for (uint8_t i = 0; i < SHAPE_MODE_MAX; i++) {
        uint8_t mode = SHAPE_MODES[shape][i];
        if (mode == MODE_NONE) {
            break;
        }

        uint8_t operand_bytes = MODE_LENGTH[mode] - 1;
        bool relative = (mode == Relative8 || mode == Relative16);
        if (want_far && mode == Absolute) {
            operand_bytes = 4; /* a far JMP/JSR carries a 32-bit target */
        }

        /* BBRn spends its second operand byte on the branch, so its value is a
         * zero-page one however long the instruction is. */
        uint8_t value_bytes = (mode == BitZeroPageRelative) ? 1 : operand_bytes;
        if (!relative) {
            /* Reject a value too wide for the form rather than truncating it,
             * and let four written digits ask for the wide form regardless. */
            if (value_bytes < 4 && value >= (1UL << (value_bytes * 8))) {
                continue;
            }
            if (value_bytes == 1 && digits > 2) {
                continue;
            }
        }

        uint8_t opcode;
        if (!find_opcode(mnemonic_index, mode, bit, &opcode)) {
            continue;
        }
        if (want_quad && !has_quad_form(opcode)) {
            continue;
        }
        if (want_far && !has_far_form(opcode)) {
            continue;
        }
        if (flat && !is_indirect_zp_z(opcode)) {
            continue;
        }

        uint8_t at = 0;
        if (want_quad) {
            bytes[at++] = OPCODE_NEG;
            bytes[at++] = OPCODE_NEG;
        } else if (want_far) {
            bytes[at++] = OPCODE_CLD;
            bytes[at++] = OPCODE_CLD;
        }
        if (flat) {
            bytes[at++] = OPCODE_EOM;
        }
        bytes[at++] = opcode;

        if (relative) {
            uint16_t offset;
            /* Out of reach means try the wider form: every 8-bit branch has a
             * 16-bit counterpart later in the list, and that one always fits. */
            if (!branch_offset(address, at + operand_bytes, value, mode == Relative16, &offset)) {
                continue;
            }
            value = offset;
        } else if (mode == BitZeroPageRelative) {
            bytes[at++] = (uint8_t)value;
            uint16_t offset;
            /* BBRn has no wider form, so this one really is out of reach. */
            if (!branch_offset(address, at + 1, second, false, &offset)) {
                return AssembleBranchTooFar;
            }
            value = offset;
            operand_bytes = 1;
        }

        for (uint8_t i = 0; i < operand_bytes; i++) {
            bytes[at++] = (uint8_t)(value >> (i * 8));
        }
        *length = at;
        return AssembleOk;
    }
    return AssembleWrongOperand;
}
