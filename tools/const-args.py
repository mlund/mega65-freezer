#!/usr/bin/env python3
"""Find calls whose arguments were known before the program ran.

A and X carry the first two argument bytes free; every byte after them costs
about four bytes of code at the call site, because the zero page has no move
and no store-immediate.  Where those bytes are *constants* the compiler is
sometimes able to fold them into the callee and leave a bare ``jsr`` -- it does
exactly that for ``lcopy_out``, whose sites all pass the same addresses -- and
sometimes it is not, and the constants are rebuilt at every site instead.

This counts the second case.  For each ``jsr`` it walks backwards over the
argument setup, records the immediates it finds, and groups the sites of each
callee by the constants they pass.  Two sites passing an identical set are the
signal worth acting on: one of them is redundant, and the callee could hold
what both were told.

Run against the linked images, not the object files -- the backend runs inside
``lld`` under LTO, so only the final ELF shows what was generated.

    tools/const-args.py                       # summary for build/src/*.M65.elf
    tools/const-args.py --json > before.json  # machine-diffable snapshot
    tools/const-args.py build-alt/src/*.elf

The backward walk is a dataflow slice, not a syntactic run: an instruction
counts only where it writes an argument slot -- A, X, or __rc2 upwards -- that
nothing later overwrote, or produces a register such an instruction reads.  A
store to anything else ends the walk, which is what keeps a local written just
before the call from being read as an argument.  Taking any instruction that
looked like setup measured 66% more bytes and invented repeat groups that the
disassembly does not support.

It still undercounts what the scheduler interleaved, and a slice is not proof
that a constant could move into the callee.  It bounds the opportunity; check a
candidate against the disassembly, then change the signature and measure.
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import shutil
import subprocess
import sys

MCPU = "mos45gs02"

INSN_RE = re.compile(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(\w+)\s*(.*?)(?:\s*;.*)?$")
LABEL_RE = re.compile(r"^[0-9a-f]{8} <(.+)>:")
CALL_RE = re.compile(r"^\$[0-9a-f]+ <(.+)>$")
IMM_RE = re.compile(r"^#\$([0-9a-f]+)$")
ZP_RE = re.compile(r"^\$([0-9a-f]{1,2})$")


def zp_target(arg: str) -> int | None:
    m = ZP_RE.match(arg)
    return int(m.group(1), 16) if m else None

# What argument setup is made of: an immediate into a register, or a register
# into the zero page.  A load from memory is setup too, but of a runtime value.
LOAD_DST = {"lda": "a", "ldx": "x", "ldy": "y", "ldz": "z"}
STORE_SRC = {"sta": "a", "stx": "x", "sty": "y", "stz": None}
TRANSFER = {"tax": ("x", "a"), "tay": ("y", "a"), "txa": ("a", "x"),
            "tya": ("a", "y"), "taz": ("z", "a"), "tza": ("a", "z")}

# Where arguments go: A and X carry the first two bytes, then __rc2 upwards.
# __rc20 and above are callee-saved and never argument slots.
FIRST_ARG_REG, LAST_ARG_REG = 2, 19

WINDOW = 48  # instructions; longer than any real argument list


class Insn:
    __slots__ = ("op", "arg", "size")

    def __init__(self, op: str, arg: str, size: int) -> None:
        self.op, self.arg, self.size = op, arg, size


def tool(name: str) -> str:
    found = shutil.which(name) or shutil.which(
        os.path.expanduser(f"~/llvm-sdk-hyppo-kernal/bin/{name}"))
    if not found:
        sys.exit(f"{name} not on PATH")
    return found


def disassemble(path: str) -> list[tuple[str, list[Insn]]]:
    """Each function, in order, with its instructions."""
    out = subprocess.run(
        [tool("llvm-objdump"), "-d", f"--mcpu={MCPU}", path],
        capture_output=True, text=True).stdout
    funcs: list[tuple[str, list[Insn]]] = []
    cur: list[Insn] = []
    for line in out.splitlines():
        label = LABEL_RE.match(line)
        if label:
            cur = []
            funcs.append((label.group(1), cur))
            continue
        m = INSN_RE.match(line)
        if m and cur is not None:
            cur.append(Insn(m.group(3), m.group(4).strip(), len(m.group(2).split())))
    return funcs


def imaginary(path: str) -> set[int]:
    """Zero-page addresses of the imaginary registers that carry arguments."""
    out = subprocess.run([tool("llvm-nm"), path], capture_output=True, text=True).stdout
    slots = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1].startswith("__rc"):
            index = parts[-1][4:]
            if index.isdigit() and FIRST_ARG_REG <= int(index) <= LAST_ARG_REG:
                slots.add(int(parts[0], 16))
    return slots


def setup_before(insns: list[Insn], at: int, slots: set[int]) -> list[Insn]:
    """A backward slice of the instructions that define this call's arguments.

    An instruction is taken only where it writes an argument slot not already
    written, or produces a register such an instruction consumes.  Anything
    else stops the walk -- otherwise a store to a local that happens to sit
    before the call is read as an argument, and the value loaded for it is
    counted as a constant the callee was told.
    """
    taken: list[Insn] = []
    needed: set[str] = set()   # registers whose value some accepted store reads
    written: set[int] = set()  # argument slots already defined
    free = {"a", "x"}          # the two bytes the ABI passes in registers
    i = at - 1
    while i >= 0 and at - i <= WINDOW:
        insn = insns[i]
        keep = False
        if insn.op in STORE_SRC:
            zp = zp_target(insn.arg)
            if zp is not None and zp in slots and zp not in written:
                written.add(zp)
                source = STORE_SRC[insn.op]
                if source:
                    needed.add(source)
                keep = True
        elif insn.op in LOAD_DST:
            dst = LOAD_DST[insn.op]
            if dst in needed or dst in free:
                needed.discard(dst)
                free.discard(dst)
                keep = True
        elif insn.op in TRANSFER:
            dst, source = TRANSFER[insn.op]
            if dst in needed or dst in free:
                needed.discard(dst)
                free.discard(dst)
                needed.add(source)
                keep = True
        if not keep:
            break
        taken.append(insn)
        i -= 1
    return list(reversed(taken))


def analyse(paths: list[str]) -> dict:
    callees: dict[str, dict] = {}
    for path in paths:
        image = os.path.basename(path).split(".")[0]
        slots = imaginary(path)
        for caller, insns in disassemble(path):
            for i, insn in enumerate(insns):
                if insn.op != "jsr":
                    continue
                name = CALL_RE.match(insn.arg)
                if not name:
                    continue
                run = setup_before(insns, i, slots)
                if not run:
                    continue
                imms = [IMM_RE.match(x.arg).group(1) for x in run if IMM_RE.match(x.arg)]
                loads = [x for x in run if x.op in LOAD_DST]
                # every value that reaches the callee came from an immediate
                constant = bool(loads) and all(IMM_RE.match(x.arg) for x in loads)
                entry = callees.setdefault(name.group(1), {
                    "sites": [], "bytes": 0, "constant_sites": 0})
                entry["sites"].append({
                    "image": image, "caller": caller,
                    "bytes": sum(x.size for x in run),
                    "constant": constant,
                    "signature": ",".join(imms) if constant else None,
                })
                entry["bytes"] += sum(x.size for x in run)
                entry["constant_sites"] += 1 if constant else 0

    for name, entry in callees.items():
        # Per image: each .M65 links separately, so the same constants in two
        # different tools are two different binaries and not a repeat at all.
        groups = collections.Counter(
            (s["image"], s["signature"]) for s in entry["sites"] if s["constant"])
        redundant = 0
        for (image, sig), count in groups.items():
            if count < 2:
                continue
            cost = next(s["bytes"] for s in entry["sites"]
                        if s["image"] == image and s["signature"] == sig)
            redundant += (count - 1) * cost
        entry["redundant"] = redundant
        entry["repeat_groups"] = {f"{i}:{k}": v for (i, k), v in groups.items() if v > 1}
    return callees


def report(callees: dict, top: int) -> None:
    total = sum(e["bytes"] for e in callees.values())
    const = sum(s["bytes"] for e in callees.values() for s in e["sites"] if s["constant"])
    redundant = sum(e["redundant"] for e in callees.values())
    sites = sum(len(e["sites"]) for e in callees.values())
    print(f"{sites} calls with argument setup, {total} bytes of it")
    print(f"{const} bytes ({const / total * 100:.0f}%) set up from immediates alone")
    print(f"{redundant} bytes repeat a constant set another site already passes\n")

    print(f"{'BYTES':>6}{'REDUN':>7}{'SITES':>7}{'CONST':>7}  CALLEE")
    ranked = sorted(callees.items(), key=lambda kv: -kv[1]["bytes"])
    for name, e in ranked[:top]:
        print(f"{e['bytes']:>6}{e['redundant']:>7}{len(e['sites']):>7}"
              f"{e['constant_sites']:>7}  {name}")

    repeats = sorted(((n, e) for n, e in callees.items() if e["redundant"]),
                     key=lambda kv: -kv[1]["redundant"])
    if repeats:
        print(f"\ncallees told the same constants more than once:")
        for name, e in repeats[:top]:
            for key, count in sorted(e["repeat_groups"].items(), key=lambda kv: -kv[1]):
                image, sig = key.split(":", 1)
                print(f"  {name}: {count} sites in {image} pass #{sig or '(none)'}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("images", nargs="*")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    paths = args.images or sorted(glob.glob("build/src/*.M65.elf"))
    if not paths:
        sys.exit("no images; build first or name them")
    data = analyse(paths)
    if args.json:
        json.dump(data, sys.stdout, indent=2)
    else:
        report(data, args.top)


if __name__ == "__main__":
    main()
