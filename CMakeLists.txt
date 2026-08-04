cmake_minimum_required(VERSION 3.14)
# c64, not mega65: the freezer's loader stub does `jmp $080d`, which is where
# the C64 BASIC header (SYS 2061) puts the entry point.  The mega65 platform
# emits a BASIC 65 stub -- "10 BANK 0 : 20 SYS 2071" -- whose BANK token C64
# BASIC cannot parse and whose entry is $0817.  MEGA65 features come from
# -mcpu and mega65-libc on top, as the cc65 build did with -t c64.
set(LLVM_MOS_PLATFORM c64)
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

# Set before CPMAddPackage so mega65-libc's own sources build for this CPU;
# the c64 platform would otherwise default to 6502.
# -fnonreentrant: freeze_sprited.c dispatches through a struct of function
# pointers, and an indirect call defeats the call-graph analysis that places
# stack frames statically, so everything reachable falls back to the slow
# soft stack.  Nothing here is re-entered: interrupts are masked throughout
# and helper.s's NMI handler is a bare rti.
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -mcpu=mos45gs02 -D__MEGA65__ -fnonreentrant")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=mos45gs02")

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
    # High-volume but genuine: kept out of `tidy` so it cannot bury the rest.
    add_custom_target(tidy-widening
        COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR}
                --checks=-*,bugprone-implicit-widening-of-multiplication-result
                ${TIDY_SOURCES}
        COMMENT "Checking 16-bit multiplication overflows")
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
