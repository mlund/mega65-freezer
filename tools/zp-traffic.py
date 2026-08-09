#!/usr/bin/env python3
"""Measure how many code bytes the freezer spends shuffling zero page.

On this target every value that will not fit in ``A``/``X``/``Y`` lives in the
zero page -- either in llvm-mos' imaginary register file (``__rc0``..``__rc31``)
or in a location the LTO zero-page allocator handed out.  Moving a byte between
two such locations costs a load and a store, four bytes, every time.  Counting
those pairs turns "register pressure" from a hunch into a number, and gives a
before/after metric for any compiler change that claims to reduce it.

Run against the linked images, not the object files: the freezer builds with
LTO, so the backend runs inside ``lld`` and only the final ELF reflects what was
actually generated.

    tools/zp-traffic.py                      # summary for build/src/*.M65.elf
    tools/zp-traffic.py --json > before.json # machine-diffable snapshot
    tools/zp-traffic.py build-alt/src/*.elf  # compare a second toolchain

``--json`` is the form to keep across a compiler change; the human summary
rounds and truncates.

The counts are deliberately syntactic -- adjacent instruction pairs in the
disassembly -- so they undercount anything the scheduler separated and they say
nothing about which pairs a compiler could actually remove.  They bound the
opportunity, they do not describe it; that needs MIR.
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

# "    84a: a9 0e        \tlda\t#$e" -- optional trailing "; 0x3 <__rc1>" comment.
INSN_RE = re.compile(r"\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)\s*(\w+)\s*(.*?)(?:\s*;.*)?$")
LABEL_RE = re.compile(r"^[0-9a-f]{8} <(.+)>:")
ZP_RE = re.compile(r"\$([0-9a-f]{1,2})$")

LOADS = {"lda": "a", "ldx": "x", "ldy": "y"}
STORES = {"sta": "a", "stx": "x", "sty": "y"}

# Pairs that a single existing 65CE02/45GS02 instruction could replace, kept for
# calibration: they are what "teach the selector one more instruction" is worth.
ISA_PAIRS = {
    ("txa", "sta", ",y"): "txa;sta abs,y -> stx abs,y",
    ("tya", "sta", ",x"): "tya;sta abs,x -> sty abs,x",
}


class Insn:
    __slots__ = ("size", "op", "arg", "func")

    def __init__(self, size: int, op: str, arg: str, func: str) -> None:
        self.size, self.op, self.arg, self.func = size, op, arg, func

    @property
    def zp(self) -> int | None:
        """The zero-page address this operand names, if it names one directly."""
        m = ZP_RE.fullmatch(self.arg)
        return int(m.group(1), 16) if m else None


def tool(name: str) -> str:
    """Locate an llvm-mos binary, preferring the toolchain that built the image."""
    prefix = os.environ.get("LLVM_MOS", os.path.expanduser("~/llvm-mos-patched2"))
    candidate = os.path.join(prefix, "bin", name)
    if os.path.exists(candidate):
        return candidate
    found = shutil.which(name)
    if not found:
        sys.exit(f"{name} not found; set LLVM_MOS to your llvm-mos install prefix")
    return found


def zp_symbols(path: str) -> dict[int, str]:
    """Map zero-page addresses to symbol names, so __rcN can be told from the rest."""
    out = subprocess.run([tool("llvm-nm"), "-n", path], capture_output=True, text=True).stdout
    syms: dict[int, str] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if addr < 0x100:
            syms.setdefault(addr, parts[2])
    return syms


def disassemble(path: str) -> list[Insn]:
    out = subprocess.run(
        [tool("llvm-objdump"), "-d", f"--mcpu={MCPU}", path], capture_output=True, text=True
    ).stdout
    insns: list[Insn] = []
    func = "?"
    for line in out.splitlines():
        label = LABEL_RE.match(line)
        if label:
            func = label.group(1)
            continue
        m = INSN_RE.match(line)
        if m:
            insns.append(Insn(len(m.group(1).split()), m.group(2).lower(), m.group(3).strip(), func))
    return insns


def classify(addr: int, syms: dict[int, str]) -> str:
    """Bucket a zero-page address: imaginary register, named global, or anonymous."""
    name = syms.get(addr)
    if name is None:
        return "unnamed-zp"
    return "__rcN" if name.startswith("__rc") else "named"


def copy_pairs(insns: list[Insn], syms: dict[int, str]):
    """Yield (source-class, dest-class, function) for every zero-page write pair.

    A pair is a load into some register immediately followed by a store of that
    same register: either a zero-page-to-zero-page copy or a constant being
    parked in zero page.  Requiring the same register is what keeps unrelated
    neighbouring instructions out of the count.  The pair is attributed to the
    function containing its store.
    """
    for first, second in zip(insns, insns[1:]):
        reg = LOADS.get(first.op)
        if reg is None or STORES.get(second.op) != reg:
            continue
        dest = second.zp
        if dest is None:
            continue
        if first.arg.startswith("#"):
            yield "#imm", classify(dest, syms), second.func
        elif (src := first.zp) is not None:
            yield classify(src, syms), classify(dest, syms), second.func


def copy_run_widths(insns: list[Insn]) -> collections.Counter:
    """Group copies into runs whose source and destination both step by one.

    A run of width 4 is a 32-bit move and could in principle become a single
    ``ldq``/``stq`` pair; width 1 is a scalar shuffle that no wide instruction
    helps with.  The distribution decides whether wide moves are worth pursuing.
    """
    widths: collections.Counter = collections.Counter()
    i = 0
    while i + 1 < len(insns):
        first, second = insns[i], insns[i + 1]
        reg = LOADS.get(first.op)
        src, dest = first.zp, second.zp
        if reg is None or STORES.get(second.op) != reg or src is None or dest is None:
            i += 1
            continue
        width, j = 1, i + 2
        while j + 1 < len(insns):
            nxt, nxt2 = insns[j], insns[j + 1]
            nreg = LOADS.get(nxt.op)
            if (
                nreg is None
                or STORES.get(nxt2.op) != nreg
                or nxt.zp != src + width
                or nxt2.zp != dest + width
            ):
                break
            width += 1
            j += 2
        widths[width] += 1
        i = j
    return widths


def analyse(paths: list[str]) -> dict:
    totals = {"instructions": 0, "code_bytes": 0, "zp_loads_stores": 0}
    flows: collections.Counter = collections.Counter()
    widths: collections.Counter = collections.Counter()
    isa: collections.Counter = collections.Counter()
    functions: list[dict] = []

    for path in paths:
        syms = zp_symbols(path)
        insns = disassemble(path)
        image = os.path.basename(path).split(".")[0]

        totals["instructions"] += len(insns)
        totals["code_bytes"] += sum(i.size for i in insns)
        totals["zp_loads_stores"] += sum(
            1
            for i in insns
            if i.zp is not None and i.op in ("lda", "ldx", "ldy", "sta", "stx", "sty", "stz")
        )

        widths.update(copy_run_widths(insns))

        per_func: dict[str, dict] = collections.defaultdict(
            lambda: {"bytes": 0, "zp_loads_stores": 0, "copies": 0}
        )
        for i in insns:
            entry = per_func[i.func]
            entry["bytes"] += i.size
            if i.zp is not None and i.op in ("lda", "ldx", "ldy", "sta", "stx", "sty", "stz"):
                entry["zp_loads_stores"] += 1

        for src_class, dest_class, func in copy_pairs(insns, syms):
            flows[f"{src_class} -> {dest_class}"] += 1
            per_func[func]["copies"] += 1

        for first, second in zip(insns, insns[1:]):
            for (op1, op2, suffix), label in ISA_PAIRS.items():
                if first.op == op1 and second.op == op2 and second.arg.endswith(suffix):
                    isa[label] += 1

        for name, entry in per_func.items():
            functions.append({"image": image, "function": name, **entry})

    functions.sort(key=lambda f: -f["copies"])
    return {
        "totals": totals,
        "copy_flows": dict(flows.most_common()),
        "copy_run_widths": {str(k): v for k, v in sorted(widths.items())},
        "isa_pair_opportunities": dict(isa),
        "functions": functions,
    }


def report(data: dict, top: int) -> None:
    t = data["totals"]
    total = t["code_bytes"]
    pairs = sum(data["copy_flows"].values())

    def share(b: int) -> str:
        return f"{100 * b / total:.1f}%" if total else "-"

    print(f"{t['instructions']} instructions, {total} code bytes")
    print(f"{t['zp_loads_stores']} zero-page loads/stores ({share(2 * t['zp_loads_stores'])})")
    print(f"\ncopy and immediate pairs: {pairs} = {4 * pairs} bytes ({share(4 * pairs)})\n")

    for flow, n in data["copy_flows"].items():
        print(f"  {n:6d}  {4 * n:6d} B  {flow}")

    print("\ncopy run widths (contiguous source and destination):")
    for width, n in data["copy_run_widths"].items():
        print(f"  width {width}: {n:5d} runs = {4 * n * int(width):6d} B")

    print("\nsingle-instruction ISA opportunities, for calibration:")
    for label, n in data["isa_pair_opportunities"].items() or [("none found", 0)]:
        print(f"  {n:6d}  {label}")

    print(f"\ntop {top} functions by copy/immediate bytes:")
    print(f"  {'copyB':>6} {'fnB':>6} {'zpLS':>5}  image / function")
    for f in data["functions"][:top]:
        print(
            f"  {4 * f['copies']:6d} {f['bytes']:6d} {f['zp_loads_stores']:5d}  "
            f"{f['image']} / {f['function']}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("images", nargs="*", default=None, help="ELF images (default build/src)")
    parser.add_argument("--json", action="store_true", help="emit the full result as JSON")
    parser.add_argument("--top", type=int, default=22, help="functions to list (default 22)")
    args = parser.parse_args()

    paths = sorted(args.images or glob.glob("build/src/*.M65.elf"))
    if not paths:
        sys.exit("no images found; pass paths explicitly or build first")

    data = analyse(paths)
    if args.json:
        json.dump(data, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        report(data, args.top)


if __name__ == "__main__":
    main()
