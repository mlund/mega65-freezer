cmake_minimum_required(VERSION 3.14)
# c64, not mega65: the freezer's loader stub does `jmp $080d`, which is where
# the C64 BASIC header (SYS 2061) puts the entry point.  The mega65 platform
# emits a BASIC 65 stub -- "10 BANK 0 : 20 SYS 2071" -- whose BANK token C64
# BASIC cannot parse and whose entry is $0817.  MEGA65 features come from
# -mcpu and mega65-libc on top, as the cc65 build did with -t c64.
set(LLVM_MOS_PLATFORM c64)
find_package(llvm-mos-sdk REQUIRED)
project(freezemenu LANGUAGES C CXX ASM)

# charset.c uses #embed, which is C23; without this the build warns on every
# translation unit that includes it.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The SDK's charset.h is C++20: its constexpr constructors are what turn the
# menu text into screen codes at compile time.  Only the menu tables are C++.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone llvm-mos build of the MEGA65 freezemenu.  Nothing is shared with
# the cc65 build in the parent directory; src/ holds its own copy of every
# source.
#
# Needs a patched llvm-mos -- see C_MCPU below for what stock clang 23 gets
# wrong.  The patched install (compiler plus a matching SDK rebuild) lives at
# /Users/mikael/llvm-mos-patched:
#
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=/Users/mikael/llvm-mos-patched/lib/cmake/llvm-mos-sdk/llvm-mos-toolchain.cmake \
#     -Dllvm-mos-sdk_DIR=/Users/mikael/llvm-mos-patched/lib/cmake/llvm-mos-sdk
#   cmake --build build
#   cmake --build build --target tidy      # also: format, format-check
#
# Both variables are needed: the toolchain file alone does not set
# llvm-mos-sdk_DIR, and find_package then fails.  Nothing in git records which
# toolchain a build used -- it lives only in the CMake cache.
#
# Set CPM_SOURCE_CACHE to a directory to avoid re-fetching mega65-libc.

# Must precede any add_executable, or the targets below are absent from the
# database and clang-tidy sees only the dependency's entries.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set before CPMAddPackage so mega65-libc's own sources build for this CPU;
# the c64 platform would otherwise default to 6502.
# -fnonreentrant: sprited/editor.c dispatches through a struct of function
# pointers, and an indirect call defeats the call-graph analysis that places
# stack frames statically, so everything reachable falls back to the slow
# soft stack.  Nothing here is re-entered: interrupts are masked throughout
# and helper.s's NMI handler is a bare rti.
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -mcpu=mos45gs02 -D__MEGA65__ -fnonreentrant")
# Mirrors CMAKE_C_FLAGS.  -fno-exceptions and -fno-rtti because nothing here
# throws: charset.h compiles clean under them and still rejects a bad character
# at compile time.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=mos45gs02 -D__MEGA65__ -fnonreentrant -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=mos45gs02")

# This build assumes a patched llvm-mos.  Stock clang 23 miscompiles 16-bit loop
# counters on 45GS02 with -mlto-zp >= 8: it decrements one location with DEW
# while the loop-exit test reads another, so the bound is silently dropped and a
# bounded loop runs past its array.  It hung MAKEDISK in sdcard_writesector's
# write-verify, and it also affects the SDK's own libc, so a patched compiler
# needs a rebuilt SDK to match.
#
# -DC_MCPU=mos6502 is the escape hatch on an unpatched compiler: no INW/DEW
# exists there, at a cost of ~3.5KB.  Applied per target rather than to the
# flags above, because mega65-libc's debug.c needs 65CE02 for `ldz`.  Every
# inline asm block in our C is plain 6502, so nothing here needs the wider set.
set(C_MCPU mos45gs02 CACHE STRING "-mcpu for our C sources (mos6502 on an unpatched compiler)")

include(cmake/CPM.cmake)

# Pinned rather than tracking `development`, so a build is reproducible; bump
# deliberately.  master does not currently compile under llvm-mos.
CPMAddPackage(
    NAME mega65libc
    GITHUB_REPOSITORY mega65/mega65-libc
    GIT_TAG 49f2f11
)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Derived from the SDK package location rather than an absolute path, so
# everything follows wherever llvm-mos is installed.  Used for the MEGA65
# register headers in src/ and for the clang-tidy/clang-format below, which
# must be the toolchain's own -- a system clang-tidy does not know the target.
cmake_path(GET llvm-mos-sdk_DIR PARENT_PATH _sdk_cmake)   # .../lib/cmake
cmake_path(GET _sdk_cmake PARENT_PATH _sdk_lib)           # .../lib
cmake_path(GET _sdk_lib PARENT_PATH LLVM_MOS_ROOT)        # install prefix

# OFF does not link: FREEZER needs the hyppo dirent layer, SPRITED needs conio
# and mouse.  The option is global; the dependency is per-target.
option(LINK_LIBC "Link mega65-libc" ON)

# Must be at top level and precede add_subdirectory, or tests registered in a
# subdirectory are silently dropped.
enable_testing()

add_subdirectory(src)

# Static analysis and formatting.  The .c files carry the diagnostics; headers
# are reached through them.
# GLOB_RECURSE, not GLOB: src/monitor/ was silently unchecked from the moment it
# existed, so both targets passed while the files being changed went unread.
file(GLOB_RECURSE TIDY_SOURCES ${SRC}/*.c ${SRC}/*.cpp)
file(GLOB_RECURSE FORMAT_SOURCES ${SRC}/*.c ${SRC}/*.cpp ${SRC}/*.h)
# Taken from the SDK unchanged, so it is not ours to format or to lint: the
# whole point is that it is the library's own source.  See printf_sdk.cc.
list(FILTER TIDY_SOURCES EXCLUDE REGEX "/(printf_sdk\\.cc)$")
list(FILTER FORMAT_SOURCES EXCLUDE REGEX "/(printf_sdk\\.cc|util\\.h|varint\\.h)$")

find_program(CLANG_TIDY_EXE   NAMES clang-tidy   HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)
find_program(CLANG_FORMAT_EXE NAMES clang-format HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)

if(CLANG_TIDY_EXE)
    # High-volume but genuine, and excluded from `tidy` in .clang-tidy so they cannot bury the
    # rest.  Almost all are provably-safe screen arithmetic; the ones that are not are the whole
    # reason to look, since a 16-bit int overflows before it widens.  Advisory, not enforced.
    add_custom_target(tidy-arithmetic
        # `--warnings-as-errors=-*` cancels the WarningsAsErrors: '*' in .clang-tidy, which would
        # otherwise make this advisory target fail the build.  An empty value does not cancel it.
        COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} "--warnings-as-errors=-*"
                --checks=-*,bugprone-implicit-widening-of-multiplication-result,bugprone-narrowing-conversions
                ${TIDY_SOURCES}
        COMMENT "Checking 16-bit overflow and narrowing (advisory)")
    # Enumerates every #define that could be a typed constant.  Advisory, and not in
    # .clang-tidy's Checks, for three reasons: the check always proposes an enum, where the
    # convention here is constexpr for scalars and enums only for closed sets; helper.h's 39
    # macros are #included by helper.S and the assembler needs real macros; and screen.h
    # keeps four as untyped literals so 28-bit address arithmetic promotes.  As an enforced
    # check it would demand NOLINT on all of those.
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

# The 45GS02 disassembler tables are generated from llvm-mos and committed, so a
# normal build needs neither Python nor llvm-mc.  Regeneration is a rare action
# after a toolchain bump, hence a non-default target -- same shape as the
# clang-tidy and clang-format targets above.
#
# NO_DEFAULT_PATH is load-bearing: the tables must come from the *patched* SDK
# this project builds with, not from a stock llvm-mc that happens to be on PATH.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(LLVM_MC_EXE NAMES llvm-mc HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)

if(Python3_FOUND AND LLVM_MC_EXE)
    add_custom_target(disasm-tables
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_disasm_tables.py
                --llvm-mc ${LLVM_MC_EXE} -o ${SRC}/monitor/disasm_tables.inc
        COMMENT "Regenerating 45GS02 disassembler tables")

    # Compiles src/monitor/disasm.c for the host and diffs every opcode/prefix
    # combination against llvm-mc, so a toolchain bump cannot silently change
    # the tables.  Needs a host compiler as well, so it is registered only when
    # one is present rather than failing the configure.
    find_program(HOST_C_COMPILER NAMES cc clang gcc)
    if(HOST_C_COMPILER)
        add_test(NAME disasm
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_disasm.py
                    --llvm-mc ${LLVM_MC_EXE} --cc ${HOST_C_COMPILER})

        # blockmove.c takes its two memory accesses from the caller, so the
        # host build drives it against a flat array.  The overlap cases are
        # the point: a forward copy into a destination inside the source range
        # corrupts only the tail, which reads as working on hardware.
        add_test(NAME blockmove
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_blockmove.py
                    --cc ${HOST_C_COMPILER})

        # Walking a real ROM covers what synthetic opcodes cannot: our decoder
        # picks the instruction boundaries, so a wrong length lands the next
        # decode mid-instruction and the disagreement cascades.  The image is
        # not ours to redistribute, so point at your own copy with
        # -DMEGA65_ROM=/path/to/MEGA65.ROM or the MEGA65_ROM environment
        # variable; without one the test simply does not exist.
        # Xemu tests.  The program checks its own invariants and quits the
        # emulator with the failing check's number, so the exit status is the
        # verdict -- nothing is parsed.  `-testing` is what makes that $D6CF
        # write work; without it the program cannot report anything.
        #
        # Locator from mega65-libc's tests/CMakeLists.txt so the two projects
        # agree, plus HINTS: on macOS Xemu installs as an app bundle and is
        # never on PATH, so a bare find_program silently registers no tests.
        find_program(XEMU NAMES xmega65 xmega65.native xmega65.exe xemu-xmega65 xemu-xmega65.exe
                     HINTS $ENV{HOME}/bin /Applications/xmega65.app/Contents/MacOS)
        if(XEMU AND TARGET test_printf)
            add_test(NAME printf
                COMMAND ${XEMU} -headless -sleepless -testing -model 3
                        -hyperserialascii -besure
                        -prg $<TARGET_FILE:test_printf>)
            # It exits in about a second; the timeout only turns a hang into a
            # reported failure rather than a stalled suite.
            set_tests_properties(printf PROPERTIES TIMEOUT 15)
        endif()

        if(XEMU AND TARGET test_dma)
            add_test(NAME dma
                COMMAND ${XEMU} -headless -sleepless -testing -model 3
                        -hyperserialascii -besure
                        -prg $<TARGET_FILE:test_dma>)
            # It exits by itself in about a second; the timeout is only so a
            # hang is reported as one rather than stalling the suite.
            set_tests_properties(dma PROPERTIES TIMEOUT 60)
        endif()

        # Covers the freezer's launch path -- region list, find, load, run --
        # which `-prg` cannot reach because a tool dropped into memory cold
        # never starts.  The tool that comes up is the SD image's rather than
        # ours; see test/verify_launch.py.  Needs Python for the keyboard.
        if(XEMU AND Python3_FOUND)
            add_test(NAME launch_monitor
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_launch.py
                        --emulator ${XEMU} --prg $<TARGET_FILE:FREEZER>
                        --keys m --expect "PC   IRQ  NMI")
            # Boots, waits for the menu, types, waits again: about half a minute.
            set_tests_properties(launch_monitor PROPERTIES TIMEOUT 180)
        endif()

        # Assemble in the monitor and read the bytes back off the card, which
        # needs an SD image: the freezer loads its tools from there, and Xemu's
        # own image carries stock MEGA65 binaries, so without one this would
        # test upstream's monitor rather than ours.  The image is cloned before
        # our builds go in, so yours is never written to.  None is committed --
        # such an image contains MEGA65.ROM.  macOS only; the clone-and-inject
        # step uses hdiutil.
        set(MEGA65_SDIMG "$ENV{MEGA65_SDIMG}" CACHE FILEPATH
            "SD image to clone for the monitor test, e.g. Xemu's own mega65.img")
        if(XEMU AND Python3_FOUND AND MEGA65_SDIMG AND EXISTS ${MEGA65_SDIMG})
            add_test(NAME monitor_assemble
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_monitor.py
                        --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                        --build $<TARGET_FILE_DIR:MONITOR>)
            set_tests_properties(monitor_assemble PROPERTIES TIMEOUT 300)
        endif()

        set(MEGA65_ROM "$ENV{MEGA65_ROM}" CACHE FILEPATH
            "MEGA65.ROM image for the disassembler's real-code test")
        if(MEGA65_ROM AND EXISTS ${MEGA65_ROM})
            add_test(NAME disasm_rom
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_disasm.py
                        --llvm-mc ${LLVM_MC_EXE} --cc ${HOST_C_COMPILER}
                        --rom ${MEGA65_ROM})
        endif()
    endif()
endif()
