---
name: cc65-to-llvm-porting
description: Porting a cc65 6502 project to llvm-mos - the bug classes cc65 hides, and how to find them. Use when moving C sources, ca65 assembly or linker layout from cc65/ca65 to llvm-mos/clang, or when a ported program misbehaves on hardware although the sources look identical to the original.
---

# Porting cc65 projects to llvm-mos

The sources compile almost unchanged. That is the trap. cc65 is a
non-optimising compiler for a machine with no memory model to speak of, so a
large body of working 6502 C quietly depends on things llvm-mos does not
promise. The port then fails on hardware while the C reads identically to the
original.

The failures in this document were all silent, intermittent, and looked like
several unrelated bugs.

## The core asymmetry

**cc65 does not optimise hard enough to punish you. LLVM does.** Nearly every
rule below reduces to one thing: tell the compiler the truth about memory it
cannot see, and about what your assembly touches.

Platform libraries that support both toolchains often encode this directly —
a declaration guarded by `#ifdef __clang__` that adds `volatile` only for the
optimising compiler is this whole document in one line. If you find such a
guard in a library you depend on, take it as a map of what your own code needs.

## 1. Memory shared with a non-CPU agent must be volatile

DMA controllers, ROM/supervisor calls, coprocessors and blitters read and
write memory the compiler cannot see. If nothing in C reads an object, its
stores are dead by the optimiser's reckoning.

The canonical failure is a descriptor populated in memory, then triggered by a
store to a hardware register:

```c
job.count = n;              /* plain stores ... */
job.dest  = addr;
POKE(TRIGGER_REG, go);      /* volatile store to MMIO */
```

A volatile store to a hardware register **does not order plain stores to other
objects**. Two back-to-back operations therefore emit *one* descriptor and
*two* triggers — the second population overwrites the first before any
observable read, so the first is deleted as dead. The first operation then
runs on whatever the previous job left behind.

Mark the descriptor `volatile`. Do the same for any byte the agent writes back
(a staging variable, a status word), or a plain read of it may be folded to
the value C last stored there.

**Verify rather than assume** — count populations against triggers in the
disassembly; they must be equal:

```sh
llvm-objdump -d --print-imm-hex prog.elf | grep -c '<job>'        # populations
llvm-objdump -d --print-imm-hex prog.elf | grep -c 'trigger_addr' # triggers
```

## 2. Buffers handed to an agent need a barrier, not just volatile

`volatile` on the descriptor protects the descriptor. It does nothing for the
buffer the agent reads or writes. A buffer filled by C and then consumed only
by the agent has no observable reader either.

Put a barrier in the one function that triggers the transfer:

```c
void start_job(void) {
    __asm__ volatile("" ::: "memory");   /* the agent touches memory here */
    POKE(TRIGGER_REG, go);
}
```

Such stores often survive without it, because dead-store elimination must
prove the store is overwritten before any read, and loops plus `ptrtoint`
escapes defeat that. Surviving by luck is not surviving. Budget a few percent
of code size.

## 3. Inline assembly must be `volatile` and declare its clobbers

`__asm__("sei")` is not a barrier, not volatile, and claims to touch nothing.
The compiler may delete it, hoist it out of a loop, or keep a live value in a
register across it.

```c
__asm__ volatile("sei" ::: "memory");
__asm__ volatile("jsr $ffd2" ::: "a", "x", "y", "p");
```

Declare every register and flag the block touches. `N` and `Z` are never
tracked — never list them. `C` and `V` are: use `"c"`, `"v"` or `"p"`. Any
`jsr` into code you don't control clobbers flags, so `"p"` belongs on
essentially every ROM call.

This class hides until LTO, because cross-module inlining is what finally lets
the optimiser act on the lie.

## 4. Do not write into string literals

cc65 puts literals in RAM and lets you scribble on them. That is undefined
behaviour, and LLVM may constant-fold a later read from the literal's known
contents and never see the store.

```c
unsigned char *menu = (unsigned char *)"....";   /* then menu[i] = x;  UB */
unsigned char  menu[] = "....";                  /* correct */
```

Build with `-Wwrite-strings` so it cannot come back.

## 5. Assembly conventions that were free under ca65

Hand-written assembly ported from ca65 keeps working right up until the
compiler's own code disagrees with it.

- **CPU state the compiler cannot name.** Some 6502 derivatives add registers
  that an addressing mode uses *implicitly*, so compiler-generated pointer
  dereferences depend on them being zero even though no compiler-generated
  code ever sets them. ca65 output never used those modes, so hand assembly
  could leave the register dirty harmlessly; llvm-mos output uses them
  constantly, and a dirty register silently offsets memory accesses
  program-wide with no fault at the site. There is typically **no** toolchain
  support here — no constraint, no clobber entry, no diagnostic — so an
  explicit reset before every `rts` back into C is the only mechanism. Check
  your target's ABI notes for which registers this applies to.
- **Mark assembly-implemented functions `__attribute__((leaf))`** unless they
  really can re-enter C. Otherwise an opaque `jsr` forces every caller off its
  statically allocated frame onto the soft stack.
- **Rename `.s` to `.S`.** Only the uppercase extension is run through the C
  preprocessor. ca65 has none, so cc65 projects keep shared constants in an
  `.inc` of `.equ`s and pull it in with `.include`; keeping the lowercase name
  keeps that limitation, and the C side needs its own hand-maintained mirror of
  every constant. Under `.S` one header of `#define`s serves both, and the
  mirror can be deleted. Convert the header but forget the extension and the
  build still succeeds: the assembler reads `#define` as a comment, every
  constant becomes an undefined external, and the first complaint is
  `undefined symbol` from the linker. Constants shared with assembly must stay
  unparenthesised — `(expr)` is indirect addressing, so `#define BUF (0x400 +
  0x40)` assembles `sta BUF` to something else entirely.
- **Split the file's sections.** ca65 encourages one segment per file, and
  `.section .text` at the top of a ported `.s` reproduces it: the unit is
  indivisible, so every target that calls one routine links all of them. Give
  each entry point `.section .text.<name>,"ax",@progbits` and each datum
  `.bss.<name>` / `.zp.bss.<name>` / `.data.<name>`; the linker's collection
  patterns already match the suffixes. Then confirm `--gc-sections` is on the
  link line — it is not implied by the optimisation level. Compiled C is
  unaffected: per-function sections are the default there. This is granularity
  for the linker, not a hint to the compiler; it has no bearing on inlining.
- **Assert struct layout rather than trusting it.** cc65 packs everything at
  alignment 1; confirm the new toolchain agrees before blaming anything else:

  ```c
  _Static_assert(__builtin_offsetof(struct job, count) == 7, "layout");
  _Static_assert(sizeof(struct job) == 18, "size");
  ```

## 6. Linker layout: entry points are load-bearing

This class costs the most and produces the strangest symptoms.

cc65 targets place `_start` at a fixed, well-known address, and loaders,
chain-loaders and launchers routinely hard-code it. Adding *anything* ahead of
the code — a version stamp, a signature, a header — shifts `_start`. Generated
headers are updated to match; hard-coded jumps elsewhere are not.

The program then **executes your data as machine code** and reaches `_start`
only by falling through it. If that data is a version string, the executed
bytes change with every commit, so the failure changes character build to
build — which is what makes it so hard to recognise. A real example decoded:

```
56 3a   LSR $3a,X      writes zero page $3a+X
32 30   AND ($30)      reads through zero-page pointer $30
```

Emit a real trampoline instead of relying on layout:

```
SECTIONS {
    .entrytramp : { KEEP(*(.entrytramp)) }   /* jmp _start */
    .version    : { KEEP(*(.version)) }
} INSERT AFTER <the platform's generated header section>
```

Order matters: the trampoline must come first, so the fixed address every
loader knows holds a jump rather than the first byte of your payload.

**Check the first bytes of the binary early.** `xxd -l 16 prog.prg` shows the
generated startup's jump target; if it disagrees with what any loader jumps
to, fix that before debugging anything else.

## 7. `-fno-lto` "fixing" it is evidence, not a fix

When a bug disappears without LTO the instinct is to ship `-fno-lto`. Resist
it — you have learned something about *visibility*, not about the bug.

LTO changes four things: cross-module optimisation, zero-page allocation,
whole-program static stack allocation, and layout. Critically, **initialised
zero-page data exists only under LTO** (a `.zp.data` section copied by a
startup routine). So any bug that scribbles on low memory can be completely
invisible without LTO and fatal with it — not because LTO introduced it, but
because LTO put something valuable where the damage lands.

Use the difference as a bisection axis: it changes codegen without changing
the program, unlike editing sources to probe.

## 8. Size: `volatile` is nearly free, inlining it is not

`volatile` means "do not merge, reorder or elide", so the optimiser cannot
compact a descriptor population — but it will happily *inline* it at every
call site. One measured project had 46 inlined populations in a single binary
at ~13 volatile stores each.

```c
__attribute__((noinline)) void copy(long src, long dst, unsigned n);
```

| variant | total, 7 binaries |
|---|---|
| no volatile (broken) | 112914 |
| + volatile, inlined | 116862 |
| + volatile, `noinline` | **112261** |

Out of line, correctness cost **23 bytes** across seven binaries, and the
result was smaller than the buggy build. A `jsr`/`rts` is nothing against a
DMA transfer.

The same trade decides how to package inline assembly. Inline asm states
exactly what it clobbers, where a C prototype can only promise the ABI's whole
clobber set — but pasted at N call sites it costs N copies of the body against
one `jsr` each. Take both: put the asm in a `static` function and choose the
inlining yourself, keyed to the optimisation mode.

```c
#if defined(__OPTIMIZE_SIZE__)          /* -Os and -Oz, not -O2 */
#define ASM_FN static inline __attribute__((noinline))
#else
#define ASM_FN static inline __attribute__((always_inline))
#endif
```

Do not leave it to LTO: measured on a three-call-site wrapper, explicit
`noinline` beat both forced inlining and the LTO inliner's own choice. The gain
is in the body — constraints let the compiler deliver arguments in whichever
registers the routine wants, so the `phx/tay/pla/tax` shuffle a hand-written
version needs to get them out of the C ABI collapses, and a constant argument
can fold into immediate loads. A `.s` file can never do that: it is machine
code by the time LTO runs.

Related levers, usually worth taking: `-fnonreentrant` when function pointers
defeat the call-graph analysis, and `leaf` per §5. **Measure `-Oz` rather than
assuming it matches `-Os`** — on one seven-binary project it was worth 7871
bytes, 7.3%, with no other change. The machine outliner is not plumbed through,
so don't expect help there.

## 9. Character encoding

ca65 translates string literals in *assembly* to PETSCII; llvm-mos does not.
cc65 translates C literals unless `#pragma charmap` says otherwise. Anything
that scans memory for a literal string, or compares text byte-for-byte against
data produced by the old toolchain, needs its case and encoding checked
explicitly.

## 10. Toolchain and build-system traps

- Writing the CPU port at address 0 is a store through a null pointer. Clang
  treats the enclosing code as unreachable UB and deletes it — entire
  functions vanish. Build with `-fno-delete-null-pointer-checks`.
- `while (1);` is UB and gets deleted. Write `for (;;) __asm__ volatile("");`.
- `-mlto-zp=<n>` is a **link-time** option; passed as a compile option it is
  silently ignored.
- In CMake, `add_link_options` appends *after* `CMAKE_EXE_LINKER_FLAGS`, so it
  silently overrides flags passed on the command line — an experiment can
  quietly never run.
- Set `CMAKE_EXPORT_COMPILE_COMMANDS` before any `add_executable`, or
  clang-tidy sees only your dependencies.
- CMake deduplicates repeated `-include`; use `SHELL:` to keep both.
- Under the default LTO, `-S` emits LLVM IR. Add `-fno-lto` to read 6502.
- Some inline-asm mistakes crash the compiler rather than diagnosing:
  `asm goto`, the `"g"` constraint, a miscased constraint, and `"=c"` as an
  output. Under LTO these surface at *link* time as
  `LLVM ERROR: unable to translate instruction`, which looks like a linker
  problem. Suspect the constraint list first.

## 11. Method — how to actually find these

They are silent and layout-sensitive; reading the C will not find them. Keep
the cc65 build alive as a reference and diff against it.

- **Diff at a level that can falsify.** Comparing register *addresses* between
  old and new will report "identical" while a wrong *value* sits in the diff.
  Compare `(address, value)` pairs.
- **Diff behaviour, not just code.** Run both binaries in a headless emulator
  and diff the screen dumps. This pinpointed a memory fill running with the
  wrong value in minutes, after hours of fruitless source reading.
- **Beware probes that change what they measure.** These bugs move with binary
  layout, so a harness that adds or removes code relocates everything, and
  successive harnesses will contradict each other. Prefer axes that leave the
  program identical (`-flto` vs `-fno-lto`, `-Os` vs `-O0`).
- **Distrust clean sweeps.** Confirm the tool examined anything at all. A
  colour-coded `ls` once fed ANSI escapes into filenames, every path failed to
  resolve, and the run reported a clean sweep over zero files.
- **Change one thing per hardware test**, and hash-compare before deploying —
  an "experiment" that turns out byte-identical destroys a known-good state
  for no information.
- **Re-test your own fixes once the real cause is known.** Fixes added while
  something else was corrupting memory *appear* to work. Of three such changes
  on one port, two were cargo and were later removed with no effect.
