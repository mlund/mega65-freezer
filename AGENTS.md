# startup

Upon starting a new session, *always* do this:

- load `llvm-mos` and `mega65-dev` skills from https://github.com/mlund/claude-skills
- if not already known, prompt user for paths to:
  - `llvm-mos-sdk` (clang compiler and tools),
  - `mega65-core/`
  - `mega65-rom` (user may/may not have access)
  - `mega65-user-guide/`
  (last three from the official mega65 gh repos).

# rules

- code docs never tracks history, are consice/terse/brief and focus on why over what. Functions and headers are exeptions and must briefly state that they do.
- every file opens with a brief comment saying what it holds. Files under MIT OR Apache-2.0 carry `// Copyright 2026 Mikael Lund aka Wombat` beneath the SPDX line; the rest of the tree is GPL-3.
- low .M65 tool byte count is a design goal - measure rather than guess, and end commit messages with the measured byte delta. See git log for history of byte hunting.
- commit messages should be brief, concise and do not litter with authorship banners and promotion; use plain 1990-era language, no modern jargon
- use idiomatic, modern C23. Names are descriptive and style enforced by clang-tidy rules.
- strive for a deep module design and recall C23 `[[nodiscard]]`. Depth often wins bytes.
- is the change testable? If not, try to make it testable. use test driven development (TDD).
- if possible use host side testing of C code - plenty of existing examples in project
- for emulated hw use Xemu with serial/hyppo testing (`scenario.py`). Also plenty of examples.
- ask before committing or pushing; let human review
- explicit addresses belong in the linker script, not in C
- When planning new features, do
  1. Claude `/code-review medium`, then
  2. `/simplify`, then
  3. code documentation review (do they adhere to guidelines give here?) right before commit
- see README.md for build and test invocation
- For bytes, prefer `uint8_t` over e.g. `unsigned char`. Same for 16 and 32 bit.

# gotchas

- force a full rebuild before quoting a byte count; a stale link reports the old number
- the freeze slot is SD sectors, not memory: edits go through the monitor's one-sector cache
- never commit plan documents, SD images or ROMs
- the screen is the C64 ROM charset - PETSCII screen codes, uppercase only, no lowercase
- measure on xemu or hardware before theorising about a hardware bug
