#!/usr/bin/env python3
"""Check the disassembler against llvm-mos, and against the core for what
llvm-mos does not know.

Four passes:

1. Round trip.  All 1024 opcode/prefix combinations -- 256 bare, 256 behind
   $EA, 256 behind $42 $42, 256 behind $42 $42 $EA -- decoded by both this
   project's disasm.c (compiled for the host) and llvm-mc, then compared.  The
   intended divergences are normalised away rather than ignored, so a new one
   shows up as a failure.

2. Branch targets.  llvm-mc prints the raw offset where we resolve the
   target, so the round trip cannot compare those operands at all.

3. Undocumented forms.  NEGQ and the $D8 $D8 far JMP/JSR/RTS prefix appear
   in neither llvm-mos nor the User Guide's tables, so there is no oracle to
   diff against.  These are hand-written vectors read off gs4510.vhdl; the
   far-JSR length is the one that matters, since getting it wrong desynchronises
   every following line.

4. A real ROM, when one is supplied.  The synthetic sweep hands llvm-mc the
   bytes of one instruction at a time; walking a ROM makes our own decoder
   choose the boundaries, so a wrong length lands the next decode
   mid-instruction and cascades -- the failure a listing actually shows.

Run via CTest, or directly:

    python3 test/verify_disasm.py --llvm-mc ~/llvm-mos-patched/bin/llvm-mc \
        --rom /path/to/MEGA65.ROM
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

TEST = Path(__file__).resolve().parent
SRC = TEST.parent / "src" / "monitor"
TOOLS = TEST.parent / "tools"

sys.path.insert(0, str(TEST))
import hostbuild  # noqa: E402

sys.path.insert(0, str(TOOLS))
# Share only the llvm-mc invocation, so a change to the triple or CPU name
# cannot leave the oracle testing a different target than the generator built
# for.  The *expectations* below are deliberately restated rather than imported:
# an oracle that takes its answers from the thing under test proves nothing.
from gen_disasm_tables import disassemble  # noqa: E402


def strip_zeros(operand: str) -> str:
    """llvm-mc prints $344 where we pad to $0344 to keep the columns aligned."""
    return re.sub(r"\$0*([0-9A-F])", r"$\1", operand)


# Branch mnemonics.  llvm-mc prints the raw offset while we resolve the target
# to a real address, so for these only the mnemonic is comparable.
BRANCHES = {
    "BPL",
    "BMI",
    "BVC",
    "BVS",
    "BCC",
    "BCS",
    "BNE",
    "BEQ",
    "BRA",
    "BSR",
    *(f"BBR{bit}" for bit in range(8)),
    *(f"BBS{bit}" for bit in range(8)),
}

# The MEGA65 User Guide spellings we deliberately prefer over llvm-mos's.
ACCUMULATOR_FORMS = {"ASL", "INC", "ROL", "DEC", "NEG", "ASR", "LSR", "ROR"}

HARNESS_BASE = 0x0001000


def run_llvm_mc(llvm_mc: str, encodings: list[list[int]]) -> list[list[str]]:
    """Decode each encoding with llvm-mc, returning the instructions it found.

    Callers pass exactly the bytes they believe one instruction occupies, so the
    *count* of instructions llvm-mc reports checks the length as well as the
    text: agreement yields one, a too-long claim yields several, and a too-short
    one fails to decode.  One invocation per encoding, because batching would
    need a delimiter and every candidate byte can also occur inside an operand.
    """
    return [disassemble(llvm_mc, encoding, hex_operands=True) for encoding in encodings]


def build_harness(workdir: Path, host_cc: str) -> Path:
    return Path(
        hostbuild.build(
            host_cc,
            str(workdir),
            "disasm_host",
            [str(TEST / "disasm_host_harness.c"), str(SRC / "disasm.c")],
            includes=[str(SRC)],
        )
    )


def run_harness(binary: Path, cases: list[list[int]]) -> list[tuple[int, str]]:
    return run_harness_lines(binary, [" ".join(f"{byte:02x}" for byte in case) for case in cases])


def run_harness_lines(binary: Path, lines: list[str]) -> list[tuple[int, str]]:
    """Decode one instruction per line; a leading @<hex> sets its address."""
    return [parse_harness_line(row) for row in hostbuild.lines(str(binary), "\n".join(lines))]


def parse_harness_line(line: str) -> tuple[int, str]:
    length, _, text = line.partition(" ")
    return int(length), text


def split_ours(text: str) -> tuple[str, str]:
    """Pull the mnemonic and operand out of one of our formatted lines."""
    match = re.match(r",[0-9A-F]{7} (?:[0-9A-F ]{2} )+ *(\S+) *(.*)", text)
    if not match:
        return ("PARSE_FAILED", text)
    return match.group(1), match.group(2).strip().replace(" ", "")


def normalise_llvm(text: str) -> tuple[str, str]:
    """Rewrite llvm-mc's output into the spellings we chose."""
    parts = text.split(None, 1)
    mnemonic = parts[0].upper()
    operand = parts[1].strip() if len(parts) > 1 else ""

    # llvm puts the RMB/SMB/BBR/BBS bit number in an operand; we put it in the
    # mnemonic, as the User Guide and the MEGA65 monitor both do.
    bit = re.match(r"\$(\d),(.*)", operand)
    if bit and mnemonic in ("RMB", "SMB", "BBR", "BBS"):
        mnemonic += bit.group(1)
        operand = bit.group(2)

    # The ten User Guide spellings.
    if mnemonic == "NOP" and not operand:
        mnemonic = "EOM"
    elif mnemonic == "RTN":
        mnemonic = "RTS"
    elif mnemonic in ACCUMULATOR_FORMS and not operand:
        operand = "A"

    operand = operand.upper().replace(" ", "").replace(",S)", ",SP)")
    return mnemonic, operand


def check_round_trip(llvm_mc: str, binary: Path) -> list[str]:
    cases: list[list[int]] = []
    for opcode in range(256):
        cases.append([opcode, 0x34, 0x12])
        cases.append([0xEA, opcode, 0x34, 0x12])
        cases.append([0x42, 0x42, opcode, 0x34, 0x12])
        cases.append([0x42, 0x42, 0xEA, opcode, 0x34, 0x12])

    ours = run_harness(binary, cases)
    # Submit only the bytes we claim the instruction occupies, so llvm-mc is
    # checking our length as well as our text.
    theirs = run_llvm_mc(
        llvm_mc, [case[:length] for case, (length, _) in zip(cases, ours, strict=True)]
    )

    failures = []
    skipped = 0
    for case, (length, text), reference in zip(cases, ours, theirs, strict=True):
        encoding = " ".join(f"{byte:02X}" for byte in case[:length])
        # $42 $42 $42 is NEGQ on real silicon but three NEGs to llvm-mos, which
        # has no entry for it.  With no oracle to diff against, the UNDOCUMENTED
        # vectors below are what check this case.
        if case[:3] == [0x42, 0x42, 0x42]:
            skipped += 1
            continue
        if len(reference) != 1:
            failures.append(
                f"{encoding}: we call this {length} bytes, llvm-mc reads it as {reference}"
            )
            continue
        our_mnemonic, our_operand = split_ours(text)
        ref_mnemonic, ref_operand = normalise_llvm(reference[0])
        if our_mnemonic != ref_mnemonic:
            failures.append(f"{encoding}: mnemonic {our_mnemonic!r} != {ref_mnemonic!r}")
        elif our_mnemonic not in BRANCHES and our_operand != ref_operand:
            failures.append(f"{encoding}: operand {our_operand!r} != {ref_operand!r}")
    print(
        f"round trip: {len(cases)} cases (text and length), {skipped} covered by "
        f"hand-written vectors instead, {len(failures)} mismatches"
    )
    return failures


# Hand-written from gs4510.vhdl.  (encoding, expected length, expected mnemonic).
# The far forms have no published mnemonic; JSRF/JMPF/RTSF is our spelling.
UNDOCUMENTED = [
    ([0x42, 0x42, 0x42], 3, "NEGQ", "gs4510.vhdl:8113 NEG NEG NEG = 32-bit NEG"),
    ([0xD8, 0xD8, 0x20, 0x78, 0x56, 0x34, 0x12], 7, "JSRF", "far JSR takes a 32-bit target"),
    ([0xD8, 0xD8, 0x4C, 0x78, 0x56, 0x34, 0x12], 7, "JMPF", "far JMP takes a 32-bit target"),
    ([0xD8, 0xD8, 0x22, 0x34, 0x12], 5, "JSRF", "far JSR indirect keeps a 16-bit vector"),
    ([0xD8, 0xD8, 0x23, 0x34, 0x12], 5, "JSRF", "far JSR indirect,X keeps a 16-bit vector"),
    ([0xD8, 0xD8, 0x6C, 0x34, 0x12], 5, "JMPF", "far JMP indirect keeps a 16-bit vector"),
    ([0xD8, 0xD8, 0x60], 3, "RTSF", "far RTS pops 32 bits, same length"),
    ([0xD8, 0xD8, 0x62, 0x34], 4, "RTSF", "far RTS #$nn"),
    # $7C is named in the VHDL comment but absent from its case list, so the
    # prefix must not survive to it: this has to decode as a plain CLD.
    ([0xD8, 0xD8, 0x7C, 0x34, 0x12], 1, "CLD", "$7C is not in the core's preserve list"),
    # A $42 $42 pair in front of an opcode with no Q form is two real NEGs.
    ([0x42, 0x42, 0x01, 0x34], 1, "NEG", "reserved Q slot stays byte-aligned"),
]


# Branch targets, which the round trip cannot check: llvm-mc prints the raw
# offset while we resolve it.  Expected values follow the assembler's own
# convention, confirmed by assembling `bsr target` with target immediately
# after -- llvm-mos encodes offset 0, so the base is the next instruction for
# both the 8- and 16-bit forms.  Addresses assume the harness base of $0001000.
BRANCH_TARGETS = [
    ([0x10, 0x00], "$0001002", "BPL +0 lands on the next instruction"),
    ([0x10, 0x7F], "$0001081", "BPL forward"),
    ([0x10, 0xFE], "$0001000", "BPL backward onto itself"),
    ([0x13, 0x00, 0x00], "$0001003", "16-bit BPL +0 lands on the next instruction"),
    ([0x63, 0xFD, 0xFF], "$0001000", "BSR backward onto itself"),
    ([0x0F, 0x12, 0x20], "$12,$0001023", "BBR0 zero page plus forward branch"),
]


def check_branch_targets(binary: Path) -> list[str]:
    cases = [case for case, _, _ in BRANCH_TARGETS]
    results = run_harness(binary, cases)

    failures = []
    for (case, want_operand, why), (_, text) in zip(BRANCH_TARGETS, results, strict=True):
        _, got_operand = split_ours(text)
        if got_operand != want_operand:
            encoding = " ".join(f"{byte:02X}" for byte in case)
            failures.append(f"{encoding}: operand {got_operand!r} != {want_operand!r} ({why})")
    print(f"branch targets: {len(BRANCH_TARGETS)} vectors, {len(failures)} mismatches")
    return failures


def check_undocumented(binary: Path) -> list[str]:
    cases = [case for case, _, _, _ in UNDOCUMENTED]
    results = run_harness(binary, cases)

    failures = []
    for (case, want_length, want_mnemonic, why), (got_length, text) in zip(
        UNDOCUMENTED, results, strict=True
    ):
        encoding = " ".join(f"{byte:02X}" for byte in case)
        got_mnemonic, _ = split_ours(text)
        if got_length != want_length:
            failures.append(f"{encoding}: length {got_length} != {want_length} ({why})")
        elif got_mnemonic != want_mnemonic:
            failures.append(f"{encoding}: mnemonic {got_mnemonic!r} != {want_mnemonic!r} ({why})")
    print(f"undocumented forms: {len(UNDOCUMENTED)} vectors, {len(failures)} mismatches")
    return failures


# CLE: one byte, no operands, not a prefix.  Appending it to an instruction lets
# one llvm-mc invocation carry thousands of them -- without a delimiter the
# results could not be mapped back, and per-instruction invocation would take
# hours over a whole ROM.
SENTINEL = 0x02


def walk_rom(binary: Path, rom: Path, base: int) -> list[tuple[int, bytes, str]]:
    """Decode a ROM image start to end, following the lengths we report."""
    image = rom.read_bytes()
    rows = hostbuild.lines(
        str(binary), "", ["--walk", str(rom), f"{base:X}", f"{base:X}", str(len(image))]
    )

    walked, address = [], base
    for row in rows:
        length, text = parse_harness_line(row)
        if length == 0:
            break
        walked.append((address, image[address - base : address - base + length], text))
        address += length
    return walked


def check_rom(llvm_mc: str, walked: list[tuple[int, bytes, str]], base: int) -> list[str]:
    """Diff every instruction of a real ROM against llvm-mc.

    Stronger than the synthetic sweep: our decoder chooses the instruction
    boundaries here, so a wrong length puts the next decode mid-instruction and
    the disagreement cascades -- which is exactly how a wrong length ruins a
    listing on screen.
    """
    ours = walked
    lines = [" ".join(f"0x{byte:02x}" for byte in [*raw, SENTINEL]) for _, raw, _ in walked]
    reference = disassemble(llvm_mc, [], hex_operands=True, stdin_text="\n".join(lines))

    failures, undocumented, index = [], 0, 0
    for addr, raw, text in ours:
        if index >= len(reference):
            break
        decoded = [reference[index]]
        index += 1
        # Anything before the sentinel beyond the first is llvm-mc splitting
        # what we call one instruction, i.e. a form it has no entry for.
        while index < len(reference) and reference[index] != "cle":
            decoded.append(reference[index])
            index += 1
        if index < len(reference) and reference[index] == "cle":
            index += 1
        if len(decoded) > 1:
            undocumented += 1
            continue

        our_mnemonic, our_operand = split_ours(text)
        ref_mnemonic, ref_operand = normalise_llvm(decoded[0])
        encoding = " ".join(f"{byte:02X}" for byte in raw)
        if our_mnemonic != ref_mnemonic:
            failures.append(f"${addr:05X} {encoding}: mnemonic {our_mnemonic} != {ref_mnemonic}")
        elif our_mnemonic not in BRANCHES and strip_zeros(our_operand) != strip_zeros(ref_operand):
            failures.append(f"${addr:05X} {encoding}: operand {our_operand} != {ref_operand}")

    quad = sum(1 for _, raw, _ in ours if raw[:2] == b"\x42\x42")
    flat = sum(1 for _, _, text in ours if "[$" in text)
    print(
        f"rom walk: {len(ours)} instructions from ${base:05X} ({quad} quad, {flat} flat, "
        f"{undocumented} undocumented), {len(failures)} disagreements"
    )
    return failures


def run_assembler(binary: Path, lines: list[str]) -> list[str]:
    """Assemble each "<address> <instruction>" line, returning bytes or !status."""
    return hostbuild.lines(str(binary), "\n".join(lines), ["--assemble"])


def check_round_trip_assembly(binary: Path, walked: list[tuple[int, bytes, str]]) -> list[str]:
    """Assemble what the disassembler printed and require the original bytes.

    The two halves share the opcode tables but not the code that reads them, so
    a mode either of them has wrong shows up here as bytes that fail to come
    back -- and unlike the llvm-mc comparison this also covers the forms
    llvm-mos has no entry for.
    """
    lines = []
    for address, _, text in walked:
        # Everything from the mnemonic on; the address and raw bytes are ours.
        mnemonic, operand = split_ours(text)
        lines.append(f"{address:X} {mnemonic} {operand}")

    differing = []
    for (address, raw, text), assembled in zip(walked, run_assembler(binary, lines), strict=True):
        want = " ".join(f"{byte:02X}" for byte in raw)
        if assembled != want:
            differing.append((address, text, want, assembled))

    # Differing bytes are not automatically wrong: a branch reachable by both
    # widths assembles to the shorter one, and the ROM does not always use it.
    # Decode what we produced and require it to say the same thing.
    failures, rewidened = [], 0
    if differing:
        # Decode at the original address: a branch operand names a target, so it
        # only reads back the same from where the instruction actually sits.
        lines = [
            f"@{address:X} {assembled}"
            for address, _, _, assembled in differing
            if assembled[:1] != "!"
        ]
        redecoded = run_harness_lines(binary, lines) if lines else []
        index = 0
        for address, text, want, assembled in differing:
            if assembled[:1] == "!":
                failures.append(f"${address:05X} {text.strip()!r}: refused with {assembled}")
                continue
            _, again = redecoded[index]
            index += 1
            if split_ours(again) == split_ours(text):
                rewidened += 1
                continue
            failures.append(f"${address:05X} {text.strip()!r}: assembled {assembled}, want {want}")

    print(
        f"assembly round trip: {len(walked)} instructions, {rewidened} branches narrowed, "
        f"{len(failures)} mismatches"
    )
    return failures


# Assembled at $1000.  Every conditional branch has a 16-bit counterpart, so
# passing beyond an 8-bit reach must widen rather than fail; BBRn has no wider
# form, which makes it the only shape that can genuinely be out of range.
ASSEMBLE_BRANCH_TOO_FAR = 4
BRANCH_RANGE = [
    ("BNE $1081", "D0 7F", "furthest forward an 8-bit branch reaches"),
    ("BNE $0F82", "D0 80", "furthest back"),
    ("BNE $1082", "D3 7F 00", "one past forward, so it widens to 16-bit"),
    ("BNE $0F81", "D3 7E FF", "one past back, likewise"),
    ("BBR0 $12,$1023", "0F 12 20", "BBRn within reach"),
    ("BBR0 $12,$1200", None, "BBRn out of reach, and it has no wider form"),
]


# Operands that must be refused rather than quietly truncated, and spellings
# that must not be mistaken for a hex value.  Assembled at $1000; "!n" is the
# AssembleStatus expected, 2 being a malformed operand and 3 one the mnemonic
# has no form for.
ASSEMBLE_EDGE = [
    ("ASL A", "0A", "the accumulator form"),
    ("ASL A ", "0A", "trailing space must not turn A into the hex digit"),
    ("LDA $12345", "!3", "too wide for absolute, so refused rather than truncated"),
    ("LDA $0012", "AD 12 00", "four digits ask for absolute"),
    ("LDA $12", "A5 12", "two digits take zero page"),
    ("BBR0 $123,$1023", "!3", "BBRn's operand is a zero-page one"),
    ("JSRF $12345678", "D8 D8 20 78 56 34 12", "a far JSR reaches past five bytes"),
    ("LDA [$12,X)", "!2", "brackets must match"),
]


def check_assembly(binary: Path) -> list[str]:
    """Assemble every opcode's own disassembly, and check the branch limits.

    Covers the opcodes a ROM happens not to contain, which is most of the
    prefixed ones.
    """
    encodings = [[opcode, 0x34, 0x12] for opcode in range(256)]
    encodings += [[0x42, 0x42, opcode, 0x34, 0x12] for opcode in range(256)]
    encodings += [[0xEA, opcode, 0x34, 0x12] for opcode in range(256)]

    failures = []
    decoded = run_harness(binary, encodings)
    lines, wanted = [], []
    for encoding, (length, text) in zip(encodings, decoded, strict=True):
        mnemonic, operand = split_ours(text)
        lines.append(f"{HARNESS_BASE:X} {mnemonic} {operand}")
        wanted.append(" ".join(f"{byte:02X}" for byte in encoding[:length]))

    for want, got, text in zip(
        wanted, run_assembler(binary, lines), [t for _, t in decoded], strict=False
    ):
        if got != want:
            failures.append(f"{text.strip()!r}: assembled {got}, want {want}")
    print(f"assembly of every opcode: {len(lines)} forms, {len(failures)} mismatches")

    branch_lines = [f"{HARNESS_BASE:X} {source}" for source, _, _ in BRANCH_RANGE]
    for (source, want, why), got in zip(
        BRANCH_RANGE, run_assembler(binary, branch_lines), strict=True
    ):
        expected = want if want is not None else f"!{ASSEMBLE_BRANCH_TOO_FAR}"
        if got != expected:
            failures.append(f"{source!r}: got {got}, want {expected} ({why})")
    edge_lines = [f"{HARNESS_BASE:X} {source}" for source, _, _ in ASSEMBLE_EDGE]
    for (source, want, why), got in zip(
        ASSEMBLE_EDGE, run_assembler(binary, edge_lines), strict=True
    ):
        if got != want:
            failures.append(f"{source!r}: got {got}, want {want} ({why})")
    print(f"branch range and edges: {len(BRANCH_RANGE) + len(ASSEMBLE_EDGE)} vectors")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-mc", required=True, help="path to the patched SDK's llvm-mc")
    parser.add_argument("--cc", default="cc", help="host compiler for the test harness")
    parser.add_argument(
        "--rom",
        type=Path,
        help="MEGA65 ROM image to walk as real code; skipped if absent",
    )
    parser.add_argument(
        "--rom-base",
        type=lambda text: int(text, 16),
        default=0x20000,
        help="physical address the ROM image loads at (hex, default 20000)",
    )
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as workdir:
        binary = build_harness(Path(workdir), args.cc)
        failures = (
            check_round_trip(args.llvm_mc, binary)
            + check_branch_targets(binary)
            + check_undocumented(binary)
        )
        failures += check_assembly(binary)
        if args.rom is not None:
            if args.rom.is_file():
                walked = walk_rom(binary, args.rom, args.rom_base)
                failures += check_rom(args.llvm_mc, walked, args.rom_base)
                failures += check_round_trip_assembly(binary, walked)
            else:
                print(f"rom walk: skipped, no image at {args.rom}")

    for failure in failures:
        print(f"  {failure}", file=sys.stderr)
    if failures:
        print(f"FAILED: {len(failures)} mismatches", file=sys.stderr)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
