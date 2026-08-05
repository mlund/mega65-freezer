# Generates version.h, defining BUILD_VERSION for src/version.c.
#
# The format is load-bearing: freeze_megainfo.c's format_util_version() scans
# memory for "v:20", skips those four bytes and parses the remainder as
# YYMMDD.HH-branch-commit.  gitversion.sh produces it.
execute_process(COMMAND ./gitversion.sh WORKING_DIRECTORY "${REPO}"
                OUTPUT_VARIABLE v OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT v)
    set(v "unknown")
endif()

# format_util_version() scans for an uppercase 'V'; llvm-mos does no case
# translation of string literals, so do it here.
string(TOUPPER "${v}" v)

file(WRITE "${OUT}" "#define BUILD_VERSION \"${v}\"\n")
