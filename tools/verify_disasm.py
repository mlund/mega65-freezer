#!/usr/bin/env python3
"""Check the disassembler against llvm-mos, and against the core for what
llvm-mos does not know.

Four passes:

1. **Round trip.**  All 1024 opcode/prefix combinations -- 256 bare, 256 behind
   $EA, 256 behind $42 $42, 256 behind $42 $42 $EA -- decoded by both this
   project's disasm.c (compiled for the host) and llvm-mc, then compared.  The
   intended divergences are normalised away rather than ignored, so a new one
   shows up as a failure.

2. **Branch targets.**  llvm-mc prints the raw offset where we resolve the
   target, so the round trip cannot compare those operands at all.

3. **Undocumented forms.**  NEGQ and the $D8 $D8 far JMP/JSR/RTS prefix appear
   in neither llvm-mos nor the User Guide's tables, so there is no oracle to
   diff against.  These are hand-written vectors read off gs4510.vhdl; the
   far-JSR length is the one that matters, since getting it wrong desynchronises
   every following line.

4. **A real ROM**, when one is supplied.  The synthetic sweep hands llvm-mc the
   bytes of one instruction at a time; walking a ROM makes our own decoder
   choose the boundaries, so a wrong length lands the next decode
   mid-instruction and cascades -- the failure a listing actually shows.

Run via CTest, or directly:

    python3 tools/verify_disasm.py --llvm-mc ~/llvm-mos-patched/bin/llvm-mc \
        --rom /path/to/MEGA65.ROM
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
SRC = TOOLS.parent / "src"

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
    """Compile disasm.c plus the host driver with the host compiler."""
    binary = workdir / "disasm_host"
    subprocess.run(
        [
            host_cc,
            "-std=c2x",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{SRC}",
            str(TOOLS / "disasm_host_harness.c"),
            str(SRC / "disasm.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    return binary


def run_harness(binary: Path, cases: list[list[int]]) -> list[tuple[int, str]]:
    payload = "\n".join(" ".join(f"{byte:02x}" for byte in case) for case in cases)
    result = subprocess.run(
        [str(binary)], input=payload, capture_output=True, text=True, check=True
    )
    return [parse_harness_line(line) for line in result.stdout.splitlines()]


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
        # has no entry for it.  There is no oracle here, so the UNDOCUMENTED
        # vectors below carry this case instead of the round trip.
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


def check_rom(llvm_mc: str, binary: Path, rom: Path, base: int) -> list[str]:
    """Walk a real ROM image and diff every instruction against llvm-mc.

    Stronger than the synthetic sweep: our decoder chooses the instruction
    boundaries here, so a wrong length puts the next decode mid-instruction and
    the disagreement cascades -- which is exactly how a wrong length ruins a
    listing on screen.
    """
    image = rom.read_bytes()
    walked = subprocess.run(
        [str(binary), "--walk", str(rom), f"{base:X}", f"{base:X}", str(len(image))],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()

    ours, lines, address = [], [], base
    for row in walked:
        length, text = parse_harness_line(row)
        if length == 0:
            break
        raw = image[address - base : address - base + length]
        ours.append((address, raw, text))
        lines.append(" ".join(f"0x{byte:02x}" for byte in [*raw, SENTINEL]))
        address += length

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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-mc", required=True, help="path to the patched SDK's llvm-mc")
    parser.add_argument("--cc", default="cc", help="host compiler for the test harness")
    parser.add_argument(
        "--rom",
        type=Path,
        help="MEGA65 ROM image to walk as real-code coverage; skipped if absent",
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
        if args.rom is not None:
            if args.rom.is_file():
                failures += check_rom(args.llvm_mc, binary, args.rom, args.rom_base)
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
