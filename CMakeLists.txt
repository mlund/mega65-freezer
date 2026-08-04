cmake_minimum_required(VERSION 3.14)
set(LLVM_MOS_PLATFORM mega65)
find_package(llvm-mos-sdk REQUIRED)
project(freezemenu LANGUAGES C ASM)

# Standalone llvm-mos build of the MEGA65 freezemenu.  Nothing is shared with
# the cc65 build in the parent directory; src/ holds its own copy of every
# source.
#
#   cmake -DCMAKE_PREFIX_PATH=$HOME/llvm-mos -B build
#   cmake --build build
#   cmake --build build --target tidy      # also: format, format-check
#
# Set CPM_SOURCE_CACHE to a directory to avoid re-fetching mega65-libc.

# Must precede any add_executable, or the targets below are absent from the
# database and clang-tidy sees only the dependency's entries.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CPM.cmake)

# Pinned rather than tracking `development`, so a build is reproducible; bump
# deliberately.  master does not currently compile under llvm-mos.
CPMAddPackage(
    NAME mega65libc
    GITHUB_REPOSITORY mega65/mega65-libc
    GIT_TAG 49f2f11
)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Re-stamp the version when HEAD moves, as the cc65 Makefile did.
execute_process(COMMAND git rev-parse --git-path HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GIT_HEAD AND NOT IS_ABSOLUTE ${GIT_HEAD})
    set(GIT_HEAD ${CMAKE_CURRENT_SOURCE_DIR}/${GIT_HEAD})
endif()

# Nothing calls into mega65-libc yet; the linkage is here so the migration of
# fdisk_hal_mega65.c onto it can proceed one function at a time.
option(LINK_LIBC "Link mega65-libc" ON)

add_subdirectory(src)

# Static analysis and formatting.  The .c files carry the diagnostics; headers
# are reached through them.
file(GLOB TIDY_SOURCES ${SRC}/*.c)
file(GLOB FORMAT_SOURCES ${SRC}/*.c ${SRC}/*.h)

find_program(CLANG_TIDY_EXE NAMES clang-tidy HINTS ${LLVM_MOS_PREFIX}/bin)
find_program(CLANG_FORMAT_EXE NAMES clang-format HINTS ${LLVM_MOS_PREFIX}/bin)

if(CLANG_TIDY_EXE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} ${TIDY_SOURCES}
        COMMENT "Running clang-tidy")
endif()

if(CLANG_FORMAT_EXE)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} --style=file -i ${FORMAT_SOURCES}
        COMMENT "Formatting with clang-format")
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --style=file --dry-run -Werror ${FORMAT_SOURCES}
        COMMENT "Checking formatting")
endif()
