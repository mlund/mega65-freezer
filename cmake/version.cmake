# Generates version.h from version.h.in, defining BUILD_VERSION.
#
# The format is load-bearing: megainfo/info.c's format_util_version() scans
# memory for "v:20", skips those four bytes and parses the remainder as
# YYMMDD.HH-branch-commit, and displays only the rightmost 25 characters.
# tools/gitversion.sh composes it; run from the repository root because it reads
# the branch and tags from git.
execute_process(COMMAND ./tools/gitversion.sh WORKING_DIRECTORY "${REPO}"
                OUTPUT_VARIABLE BUILD_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE status ERROR_QUIET)
if(NOT status EQUAL 0 OR NOT BUILD_VERSION)
    set(BUILD_VERSION "unknown")
endif()

# format_util_version() scans for an uppercase 'V'; llvm-mos does no case
# translation of string literals, so do it here.
string(TOUPPER "${BUILD_VERSION}" BUILD_VERSION)

# configure_file() rewrites only on change, so an unmoved stamp costs no relink.
configure_file("${TEMPLATE}" "${OUT}" @ONLY)
