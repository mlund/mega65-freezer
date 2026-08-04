# Stamps the build version into the binary.
#
# The format is load-bearing: freeze_megainfo.c's format_util_version() scans
# memory for "v:20", skips those four bytes and parses the remainder as
# YYMMDD.HH-branch-commit to show in the help menu.  gitversion.sh produces it.
#
# Nothing links against the symbol -- it is found by scanning -- so it needs
# `used, retain` to survive --gc-sections.
execute_process(COMMAND ./gitversion.sh WORKING_DIRECTORY "${REPO}"
                OUTPUT_VARIABLE v OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT v)
    set(v "unknown")
endif()

# ca65 maps ASCII lowercase to PETSCII 0x41-0x5A in string literals, so the
# cc65 build stored "V:..." even though gitversion.sh emits "v:...".
# format_util_version() scans for that 0x56, and llvm-mos does no such
# translation -- so uppercase it here.  For letters, digits and .-: that is
# byte-for-byte what ca65 produced.
string(TOUPPER "${v}" v)
# The freezer's loader stub does `jmp $080d` -- the entry the C64 BASIC header
# used before .version was pinned there, which pushed _start well past it.
# Without a jump at $080d the CPU executes the version string as code and only
# reaches _start by falling through it, scribbling over zero page on the way.
# Emit a real trampoline so $080d stays a valid entry point whatever the
# version string happens to spell.
file(WRITE "${OUT}"
     "__asm__(\".section .entrytramp,\\\"axR\\\",@progbits\\n\"\n"
     "        \"jmp _start\\n\");\n"
     "__attribute__((used, retain, section(\".version\"))) const char version[] = \"V:${v}\";\n")
