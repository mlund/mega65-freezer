# Generates version.h from version.h.in, defining BUILD_VERSION.
#
# The format is load-bearing: freeze_megainfo.c's format_util_version() scans
# memory for "v:20", skips those four bytes and parses the remainder as
# YYMMDD.HH-branch-commit, and displays only the rightmost 25 characters.
# Produced by this directory's gitversion.sh -- a copy of the cc65 build's, so
# the two can diverge.
execute_process(COMMAND ./gitversion.sh WORKING_DIRECTORY "${REPO}"
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
