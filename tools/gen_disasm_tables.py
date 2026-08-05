#!/usr/bin/env python3
"""Generate the 45GS02 disassembler tables for MONITOR's `D` command.

The source of truth is llvm-mos's own assembler: every opcode is decoded by
`llvm-mc -disassemble --mcpu=mos45gs02`, so the tables cannot drift from the
toolchain the project builds with.  Run via CMake rather than by hand::

    cmake --build build --target disasm-tables

which passes the patched SDK's llvm-mc.  A stock llvm-mc must not be used; see
the header comment in llvm/CMakeLists.txt.

Two departures from what llvm-mc prints, both deliberate and both applied here:

* Ten mnemonics follow the MEGA65 User Guide instead, because the monitor's
  command syntax deliberately mirrors the Matrix Mode / UART monitor.  Eight
  accumulator forms gain an explicit `A` operand, `$62` is `RTS #$nn` rather
  than `RTN`, and `$EA` is `EOM` rather than `NOP`.
* Branch operands are emitted as addressing modes, not as the raw offsets
  llvm-mc prints, so the decoder can resolve them to real addresses.

`NEGQ` ($42 $42 $42) and the `$D8 $D8` far JMP/JSR/RTS prefix are absent from
llvm-mos *and* from the User Guide's tables, so they cannot be generated from
any oracle.  They are hand-written in disasm.c; see the prefix notes there.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

# Branch opcodes.  llvm-mc prints their operand as a raw offset, which is
# indistinguishable from a zero-page or absolute operand in the output text, so
# the split has to be driven from the opcode instead of parsed back out.
RELATIVE_8 = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x80}
RELATIVE_16 = {0x13, 0x33, 0x53, 0x73, 0x93, 0xB3, 0xD3, 0xF3, 0x83, 0x63}

# The forms the User Guide writes with an explicit accumulator operand: ASL A,
# INC A, ROL A, DEC A, NEG A, ASR A, LSR A, ROR A.  llvm-mc prints them bare.
ACCUMULATOR = {0x0A, 0x1A, 0x2A, 0x3A, 0x42, 0x43, 0x4A, 0x6A}

# The remaining two User Guide spellings, keyed by opcode.
MNEMONIC_OVERRIDE = {0x62: "RTS", 0xEA: "EOM"}

# NEGQ.  The core implements $42 $42 $42 as a 32-bit NEG (gs4510.vhdl:8113),
# but it is in neither llvm-mos nor the User Guide's tables, so llvm-mc reports
# three plain NEGs and the probe below cannot find it.
QUAD_EXTRA = {0x42}

# Addressing modes, and the instruction length each implies.  Order defines the
# enum's numbering, so appending is safe and reordering is not.
MODES = [
    ("Implied", 1),
    ("Accumulator", 1),
    ("Immediate", 2),
    ("ZeroPage", 2),
    ("ZeroPageX", 2),
    ("ZeroPageY", 2),
    ("IndirectZeroPageX", 2),
    ("IndirectZeroPageY", 2),
    ("IndirectZeroPageZ", 2),
    ("StackRelativeIndirectY", 2),
    ("Absolute", 3),
    ("AbsoluteX", 3),
    ("AbsoluteY", 3),
    ("IndirectAbsolute", 3),
    ("IndirectAbsoluteX", 3),
    ("ImmediateWord", 3),
    ("Relative8", 2),
    ("Relative16", 3),
    ("BitZeroPage", 2),
    ("BitZeroPageRelative", 3),
]
MODE_INDEX = {name: i for i, (name, _) in enumerate(MODES)}
MODE_LENGTH = dict(MODES)

# llvm-mc's operand text, with the probe values substituted back out, mapped to
# the mode it denotes.  $34 and $1234 are arbitrary but must differ in width so
# that zero-page and absolute forms stay distinguishable.
OPERAND_SHAPES = {
    "": "Implied",
    "#ZP": "Immediate",
    "ZP": "ZeroPage",
    "ZP,x": "ZeroPageX",
    "ZP,y": "ZeroPageY",
    "(ZP,x)": "IndirectZeroPageX",
    "(ZP),y": "IndirectZeroPageY",
    "(ZP),z": "IndirectZeroPageZ",
    "(ZP,s),y": "StackRelativeIndirectY",
    "ABS": "Absolute",
    "ABS,x": "AbsoluteX",
    "ABS,y": "AbsoluteY",
    "(ABS)": "IndirectAbsolute",
    "(ABS,x)": "IndirectAbsoluteX",
    "#ABS": "ImmediateWord",
}

# Mnemonics that transfer control.  Used only for syntax colouring, so the set
# is about what a reader scanning for control flow wants highlighted, not about
# any hardware property.
CONTROL_FLOW = {
    "BCC",
    "BCS",
    "BEQ",
    "BMI",
    "BNE",
    "BPL",
    "BVC",
    "BVS",
    "BRA",
    "BSR",
    "BRK",
    "JMP",
    "JSR",
    "RTI",
    "RTS",
    "BBR",
    "BBS",
}

PROBE_LOW = 0x34
PROBE_HIGH = 0x12


def disassemble(
    llvm_mc: str,
    encoding: list[int],
    hex_operands: bool = False,
    stdin_text: str | None = None,
) -> list[str]:
    """Decode one byte sequence, returning llvm-mc's output lines.

    `stdin_text` submits pre-built input instead, for callers batching many
    encodings: llvm-mc decodes each input line independently.
    """
    command = [llvm_mc, "-disassemble", "-triple", "mos", "--mcpu=mos45gs02"]
    if hex_operands:
        command.append("--print-imm-hex")
    text = stdin_text if stdin_text is not None else " ".join(f"0x{byte:02x}" for byte in encoding)
    result = subprocess.run(
        command,
        input=text,
        capture_output=True,
        text=True,
        check=False,
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def decode_base_map(llvm_mc: str) -> dict[int, tuple[str, str]]:
    """Decode all 256 base opcodes into (mnemonic, mode) pairs."""
    table: dict[int, tuple[str, str]] = {}
    for opcode in range(256):
        lines = disassemble(llvm_mc, [opcode, PROBE_LOW, PROBE_HIGH])
        if not lines:
            raise SystemExit(f"llvm-mc decoded nothing for opcode ${opcode:02X}")

        parts = lines[0].split(None, 1)
        mnemonic = MNEMONIC_OVERRIDE.get(opcode, parts[0].upper())
        operand = parts[1].strip() if len(parts) > 1 else ""

        if opcode in RELATIVE_8:
            mode = "Relative8"
        elif opcode in RELATIVE_16:
            mode = "Relative16"
        elif opcode & 0x0F == 0x0F:
            mode = "BitZeroPageRelative"
        elif opcode & 0x0F == 0x07:
            mode = "BitZeroPage"
        elif opcode in ACCUMULATOR:
            mode = "Accumulator"
        else:
            shape = operand.replace(str(PROBE_HIGH * 256 + PROBE_LOW), "ABS")
            shape = shape.replace(str(PROBE_LOW), "ZP").replace(" ", "")
            if shape not in OPERAND_SHAPES:
                raise SystemExit(f"unmapped operand {operand!r} for ${opcode:02X}")
            mode = OPERAND_SHAPES[shape]

        # RMB/SMB/BBR/BBS carry a bit number, which the decoder appends from the
        # opcode rather than the table storing RMB0..RMB7 as eight mnemonics --
        # that would take the table from 91 three-byte entries to 119 four-byte
        # ones, costing about 200 bytes to say what (opcode >> 4) & 7 already says.
        table[opcode] = (mnemonic, mode)
    return table


def decode_quad_set(llvm_mc: str) -> set[int]:
    """Find the opcodes that accept the $42 $42 (Q) prefix.

    An opcode with no Q form leaves llvm-mc decoding the prefix as two plain
    NEGs, which is exactly the fallback the decoder relies on.
    """
    quad = set()
    for opcode in range(256):
        lines = disassemble(llvm_mc, [0x42, 0x42, opcode, PROBE_LOW, PROBE_HIGH])
        if lines and lines[0].split()[0] != "neg":
            quad.add(opcode)
    return quad | QUAD_EXTRA


def emit_bitmap(name: str, members: set[int], size: int, comment: str) -> list[str]:
    """Render a set of small integers as a packed bit array."""
    out = [comment, f"static const uint8_t {name}[{size}] = {{"]
    for row_start in range(0, size, 8):
        row = []
        for byte_index in range(row_start, row_start + 8):
            value = sum(1 << bit for bit in range(8) if byte_index * 8 + bit in members)
            row.append(f"0x{value:02x},")
        out.append("    " + " ".join(row))
    out.append("};")
    return out


def render(base: dict[int, tuple[str, str]], quad: set[int]) -> str:
    mnemonics = sorted({mnemonic for mnemonic, _ in base.values()})
    mnemonic_index = {name: i for i, name in enumerate(mnemonics)}
    width = max(len(name) for name in mnemonics)

    control_flow = {mnemonic_index[name] for name in mnemonics if name in CONTROL_FLOW}

    out = [
        "// Generated by tools/gen_disasm_tables.py -- do not edit.",
        "//",
        "// Regenerate with:  cmake --build build --target disasm-tables",
        "//",
        "// Decoded from llvm-mos's own assembler, with the ten MEGA65 User Guide",
        "// mnemonic spellings applied.  See the generator for the full rationale.",
        "",
        f"#define DISASM_MNEMONIC_COUNT {len(mnemonics)}",
        f"#define DISASM_MNEMONIC_WIDTH {width}",
        "",
        "// Mnemonics as character lists rather than string literals: every one is",
        "// copied out by a fixed-width loop, so a terminator would only cost bytes --",
        "// and clang rejects a NUL-less string initialiser outright",
        "// (-Wunterminated-string-initialization).",
        "static const char MNEMONIC_TEXT[DISASM_MNEMONIC_COUNT][DISASM_MNEMONIC_WIDTH] = {",
    ]
    for start in range(0, len(mnemonics), 6):
        row = ", ".join(
            "{" + ", ".join(f"'{char}'" for char in name.ljust(width)) + "}"
            for name in mnemonics[start : start + 6]
        )
        out.append(f"    {row},")
    out.append("};")
    out.append("")

    out.append("enum AddressingMode : uint8_t {")
    for name, _ in MODES:
        out.append(f"    {name},")
    out.append("};")
    out.append("")

    out.append("// Instruction length implied by each addressing mode, prefixes excluded.")
    out.append(f"static const uint8_t MODE_LENGTH[{len(MODES)}] = {{")
    out.append("    " + " ".join(f"{MODE_LENGTH[name]}," for name, _ in MODES))
    out.append("};")
    out.append("")

    out.append("static const uint8_t OPCODE_MNEMONIC[256] = {")
    for row in range(16):
        entries = (mnemonic_index[base[row * 16 + col][0]] for col in range(16))
        out.append("    " + " ".join(f"{value:3d}," for value in entries))
    out.append("};")
    out.append("")

    out.append("static const uint8_t OPCODE_MODE[256] = {")
    for row in range(16):
        entries = (MODE_INDEX[base[row * 16 + col][1]] for col in range(16))
        out.append("    " + " ".join(f"{value:2d}," for value in entries))
    out.append("};")
    out.append("")

    out += emit_bitmap(
        "QUAD_CAPABLE",
        quad,
        32,
        "// Opcodes with a Q form, indexed by opcode: bit (op & 7) of byte (op >> 3).",
    )
    out.append("")
    out += emit_bitmap(
        "CONTROL_FLOW",
        control_flow,
        (len(mnemonics) + 63) // 64 * 8,
        "// Mnemonics that transfer control, for syntax colouring only.",
    )
    out.append("")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-mc", required=True, help="path to the patched SDK's llvm-mc")
    parser.add_argument("-o", "--output", required=True, help="destination .inc file")
    args = parser.parse_args()

    base = decode_base_map(args.llvm_mc)
    quad = decode_quad_set(args.llvm_mc)

    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write(render(base, quad))

    mnemonics = {mnemonic for mnemonic, _ in base.values()}
    print(
        f"{len(mnemonics)} mnemonics, {len(quad)} Q-capable opcodes, "
        f"{len(MODES)} addressing modes -> {args.output}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
