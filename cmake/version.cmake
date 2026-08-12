# Generates version.h from version.h.in, defining BUILD_VERSION and BUILD_TAG.
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

# The release the menu shows, which is the nearest tag rather than the stamp
# above: the stamp names a build, this names a release.  --abbrev=0 keeps it to
# the bare tag, so it stays the same width between commits and the commit that
# distinguishes them is already carried by BUILD_VERSION.
execute_process(COMMAND git describe --tags --abbrev=0 WORKING_DIRECTORY "${REPO}"
                OUTPUT_VARIABLE BUILD_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE tag_status ERROR_QUIET)
if(NOT tag_status EQUAL 0 OR NOT BUILD_TAG)
    set(BUILD_TAG "untagged")
endif()
string(TOUPPER "${BUILD_TAG}" BUILD_TAG)

# configure_file() rewrites only on change, so an unmoved stamp costs no relink.
configure_file("${TEMPLATE}" "${OUT}" @ONLY)
