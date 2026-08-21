cmake_minimum_required(VERSION 3.14)
# c64, not mega65: the loader stub jumps to $080d, where the C64 BASIC header
# (SYS 2061) puts the entry point.  The mega65 platform emits a BASIC 65 stub
# whose BANK token C64 BASIC cannot parse, and enters at $0817.  MEGA65 support
# comes from -mcpu and mega65-libc on top.
set(LLVM_MOS_PLATFORM c64)
find_package(llvm-mos-sdk REQUIRED)
project(freezemenu LANGUAGES C CXX ASM)

# The SDK's toolchain defaults the build type to MinSizeRel, whose stock flags
# are "-Os -DNDEBUG".  -Oz is smaller than -Os on every target here, so it is
# the one optimisation flag rather than a second one appended after it -- and
# set at this level it reaches mega65-libc too, which add_compile_options() in
# src/ never did.
set(CMAKE_C_FLAGS_MINSIZEREL "-Oz -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG" CACHE STRING "" FORCE)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Must precede any add_executable, or our targets are absent from the database
# and clang-tidy sees only the dependency's entries.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set before CPMAddPackage so mega65-libc builds for this CPU; the c64 platform
# would default to 6502.  -fnonreentrant: sprited/editor.c dispatches through a
# struct of function pointers, and an indirect call defeats the call-graph
# analysis that places stack frames statically.  Nothing here is re-entered --
# interrupts are masked throughout and helper.S's NMI handler is a bare rti.
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -mcpu=mos45gs02 -D__MEGA65__ -fnonreentrant")
# -fno-exceptions and -fno-rtti because nothing here throws.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=mos45gs02 -D__MEGA65__ -fnonreentrant -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=mos45gs02")

set(C_MCPU mos45gs02 CACHE STRING "-mcpu for our C sources (mos6502 on an unpatched compiler)")

# On macOS Xemu installs as an app bundle and is never on PATH, so searching
# late leaves the emulator tests silently unregistered.
find_program(XEMU NAMES xmega65 xmega65.native xmega65.exe xemu-xmega65 xemu-xmega65.exe
             HINTS $ENV{HOME}/bin /Applications/xmega65.app/Contents/MacOS)

include(cmake/CPM.cmake)

CPMAddPackage(
    NAME mega65libc
    GITHUB_REPOSITORY mega65/mega65-libc
    GIT_TAG 49f2f11
)

# The host tests' framework.  DOWNLOAD_ONLY because only the header is wanted:
# adding doctest's own CMakeLists would declare targets against this build's
# compilers, which cross-compile for the 45GS02 and cannot produce anything
# that runs here.  DOCTEST_INCLUDE_DIR is what test/ compiles against.
CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.5.3
    DOWNLOAD_ONLY YES
)
if(doctest_ADDED)
    set(DOCTEST_INCLUDE_DIR ${doctest_SOURCE_DIR}/doctest)
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

cmake_path(GET llvm-mos-sdk_DIR PARENT_PATH _sdk_cmake)   # .../lib/cmake
cmake_path(GET _sdk_cmake PARENT_PATH _sdk_lib)           # .../lib
cmake_path(GET _sdk_lib PARENT_PATH LLVM_MOS_ROOT)        # install prefix

option(LINK_LIBC "Link mega65-libc" ON)

# Found here rather than where they are used: src/, tools/ and test/ all want
# them, and a find_program in a subdirectory would not be visible to its
# siblings.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(LLVM_MC_EXE NAMES llvm-mc HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)
find_program(HOST_C_COMPILER NAMES cc clang gcc)
# The doctest tests are C++ and the code they exercise stays C, so both are
# needed.  Neither can be CMAKE_C_COMPILER: this build cross-compiles for the
# 45GS02, so the project's own compilers cannot produce something that runs here.
find_program(HOST_CXX_COMPILER NAMES c++ clang++ g++)

# iomap.txt is not ours to redistribute and the table generated from it is
# committed, so a checkout without it builds normally and only loses the
# ability to regenerate.  Same shape as MEGA65_ROM in test/.
set(MEGA65_IOMAP "$ENV{MEGA65_IOMAP}" CACHE FILEPATH
    "mega65-core iomap.txt, source for the register bit-name database")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdimage.cmake)

# Must be at top level and precede add_subdirectory.
enable_testing()

add_subdirectory(src)
add_subdirectory(tools)
# After src: the emulator tests check `if(TARGET ...)` and name the tools'
# output directory, both of which need those targets to exist already.
add_subdirectory(test)

file(GLOB_RECURSE TIDY_SOURCES ${SRC}/*.c ${SRC}/*.cpp)
file(GLOB_RECURSE FORMAT_SOURCES ${SRC}/*.c ${SRC}/*.cpp ${SRC}/*.h)

find_program(CLANG_TIDY_EXE   NAMES clang-tidy   HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)
find_program(CLANG_FORMAT_EXE NAMES clang-format HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)

if(CLANG_TIDY_EXE)
    # The two advisory targets cancel .clang-tidy's WarningsAsErrors: '*' with
    # `-*`, which would otherwise fail the build.  An empty value does not
    # cancel it.
    add_custom_target(tidy-arithmetic
        COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} "--warnings-as-errors=-*"
                --checks=-*,bugprone-implicit-widening-of-multiplication-result,bugprone-narrowing-conversions
                ${TIDY_SOURCES}
        COMMENT "Checking 16-bit overflow and narrowing (advisory)")
    add_custom_target(tidy-macros
        COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} "--warnings-as-errors=-*"
                --checks=-*,modernize-macro-to-enum
                ${TIDY_SOURCES}
        COMMENT "Listing macro constants that could be typed (advisory)")
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
