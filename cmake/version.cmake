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
file(WRITE "${OUT}"
     "__attribute__((used, retain)) const char version[] = \"v:${v}\";\n")
