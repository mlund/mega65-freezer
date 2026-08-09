cmake_minimum_required(VERSION 3.14)
# c64, not mega65: the freezer's loader stub does `jmp $080d`, which is where
# the C64 BASIC header (SYS 2061) puts the entry point.  The mega65 platform
# emits a BASIC 65 stub -- "10 BANK 0 : 20 SYS 2071" -- whose BANK token C64
# BASIC cannot parse and whose entry is $0817.  MEGA65 features come from
# -mcpu and mega65-libc on top, as the cc65 build did with -t c64.
set(LLVM_MOS_PLATFORM c64)
find_package(llvm-mos-sdk REQUIRED)
project(freezemenu LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

set(C_MCPU mos45gs02 CACHE STRING "-mcpu for our C sources (mos6502 on an unpatched compiler)")

# On macOS Xemu installs as an app bundle and is never on PATH, so searching
# late leaves the dependency's five tests silently unregistered.
find_program(XEMU NAMES xmega65 xmega65.native xmega65.exe xemu-xmega65 xemu-xmega65.exe
             HINTS $ENV{HOME}/bin /Applications/xmega65.app/Contents/MacOS)

include(cmake/CPM.cmake)

CPMAddPackage(
    NAME mega65libc
    GITHUB_REPOSITORY mega65/mega65-libc
    GIT_TAG 49f2f11
)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

cmake_path(GET llvm-mos-sdk_DIR PARENT_PATH _sdk_cmake)   # .../lib/cmake
cmake_path(GET _sdk_cmake PARENT_PATH _sdk_lib)           # .../lib
cmake_path(GET _sdk_lib PARENT_PATH LLVM_MOS_ROOT)        # install prefix

option(LINK_LIBC "Link mega65-libc" ON)

# Must be at top level and precede add_subdirectory
enable_testing()

add_subdirectory(src)

file(GLOB_RECURSE TIDY_SOURCES ${SRC}/*.c ${SRC}/*.cpp)
file(GLOB_RECURSE FORMAT_SOURCES ${SRC}/*.c ${SRC}/*.cpp ${SRC}/*.h)

find_program(CLANG_TIDY_EXE   NAMES clang-tidy   HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)
find_program(CLANG_FORMAT_EXE NAMES clang-format HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)

if(CLANG_TIDY_EXE)
    add_custom_target(tidy-arithmetic
        # `--warnings-as-errors=-*` cancels the WarningsAsErrors: '*' in .clang-tidy, which would
        # otherwise make this advisory target fail the build.  An empty value does not cancel it.
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

# The 45GS02 disassembler tables are generated from llvm-mos and committed, so a
# normal build needs neither Python nor llvm-mc.  Regeneration is a rare action
# after a toolchain bump, hence a non-default target -- same shape as the
# clang-tidy and clang-format targets above.

find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(LLVM_MC_EXE NAMES llvm-mc HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)
find_program(HOST_C_COMPILER NAMES cc clang gcc)

# iomap.txt is not ours to redistribute and the generated table is committed, so
# a checkout without it builds normally and only loses the ability to
# regenerate.  Same shape as MEGA65_ROM below.
set(MEGA65_IOMAP "$ENV{MEGA65_IOMAP}" CACHE FILEPATH
    "mega65-core iomap.txt, source for the register bit-name database")
if(Python3_FOUND AND MEGA65_IOMAP AND EXISTS ${MEGA65_IOMAP})
    add_custom_target(iomap-names
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_iomap_names.py
                --iomap ${MEGA65_IOMAP} -o ${SRC}/monitor/iomap.bin
        COMMENT "Regenerating the I/O register database")
endif()

# iomap database is data on the card
add_custom_target(stage-iomap ALL
    BYPRODUCTS ${CMAKE_BINARY_DIR}/src/IOMAP.M65
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SRC}/monitor/iomap.bin ${CMAKE_BINARY_DIR}/src/IOMAP.M65
    COMMENT "Staging IOMAP.M65")

# The frames the freezer draws around a slot's thumbnail, picked by machine type
# and loaded off the card at run time.  Generated rather than committed: the
# generator is plain Python, so unlike IOMAP.M65 there is nothing to go missing.
if(Python3_FOUND)
    set(THUMB_FRAMES)
    foreach(machine m65 c65 c64)
        string(TOUPPER ${machine} MACHINE)
        set(frame ${CMAKE_BINARY_DIR}/src/${MACHINE}THUMB.M65)
        add_custom_command(OUTPUT ${frame}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_thumbnail_frames.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/assets/thumbnail-surround-${machine}.png
                    --xoff 5 --yoff 1 -o ${frame}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/thumbnail-surround-${machine}.png
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_thumbnail_frames.py
            COMMENT "Generating ${MACHINE}THUMB.M65")
        list(APPEND THUMB_FRAMES ${frame})
    endforeach()
    add_custom_target(thumbnail-frames ALL DEPENDS ${THUMB_FRAMES})
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdimage.cmake)

# The pure code is checked on the host, against Python's own arithmetic -- no
# assembler, no emulator.  Guarded on its own needs rather than on the
# disassembler's, or a machine without llvm-mc silently runs no tests at all.
if(Python3_FOUND AND HOST_C_COMPILER)
    find_program(MTOOLS_MDIR NAMES mdir)
    if(MTOOLS_MDIR AND MEGA65_SDIMG AND EXISTS ${MEGA65_SDIMG})
        add_test(NAME fat32
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_fat32.py
                    --sdimg ${MEGA65_SDIMG})
        set_tests_properties(fat32 PROPERTIES TIMEOUT 300)
    endif()

    add_test(NAME thumbnail_frames
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_thumbnail_frames.py)

    add_test(NAME format
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_format.py
                --cc ${HOST_C_COMPILER})

    # The disk geometry, the FAT records and the RTC decoding, checked against
    # what hyppo and the 45IO27 registers require rather than against this
    # code's own arithmetic.  A wrong image size is a disk that mounts and then
    # reads back the wrong half.
    add_test(NAME layout
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_layout.py
                --cc ${HOST_C_COMPILER})

    # The bit editor's record stream is variable length, so an off-by-one in the
    # walk puts every name on the wrong bit -- which on hardware reads as a bad
    # database rather than a bad decoder.  MEGA65_IOMAP adds the name diff.
    set(BITEDIT_ARGS --cc ${HOST_C_COMPILER})
    if(MEGA65_IOMAP AND EXISTS ${MEGA65_IOMAP})
        list(APPEND BITEDIT_ARGS --iomap ${MEGA65_IOMAP})
    endif()
    add_test(NAME bitedit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_bitedit.py
                ${BITEDIT_ARGS})
endif()

if(Python3_FOUND AND LLVM_MC_EXE)
    add_custom_target(disasm-tables
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_disasm_tables.py
                --llvm-mc ${LLVM_MC_EXE} -o ${SRC}/monitor/disasm_tables.inc
        COMMENT "Regenerating 45GS02 disassembler tables")

    # Compiles src/monitor/disasm.c for the host and diffs every opcode/prefix
    # combination against llvm-mc, so a toolchain bump cannot silently change
    # the tables.  Needs a host compiler as well, so it is registered only when
    # one is present rather than failing the configure.
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

        # These check their own invariants and quit the emulator with the
        # failing check's number, so the exit status is the verdict -- nothing
        # is parsed.  `-testing` is what makes that $D6CF write work.
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

        # Launch AUDIOMIX from the freeze menu and reach its advanced view.  A
        # bare `-prg` never starts it, same as MONITOR, so this needs the SD
        # image too; see test/verify_audiomix_xemu.py.
        if(XEMU AND Python3_FOUND AND MEGA65_SDIMG AND EXISTS ${MEGA65_SDIMG})
            add_test(NAME launch_audiomix
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_audiomix_xemu.py
                        --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                        --build $<TARGET_FILE_DIR:AUDIOMIX>)
            set_tests_properties(launch_audiomix PROPERTIES TIMEOUT 180)
        endif()

        # Assemble in the monitor and read the bytes back off the card.
        if(XEMU AND Python3_FOUND AND MEGA65_SDIMG AND EXISTS ${MEGA65_SDIMG})
            add_test(NAME monitor_assemble
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_monitor.py
                        --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                        --build $<TARGET_FILE_DIR:MONITOR>)
            set_tests_properties(monitor_assemble PROPERTIES TIMEOUT 300)

            # The same round trip for `B`: toggle a named bit, type a value over
            # it, leave, read somewhere else so the sector is displaced, and
            # reopen -- so the byte that comes back has been off the card.  With
            # FREEZER_TRACE the write log on the hypervisor serial channel is
            # required too, which reports each write rather than only the result.
            set(BITEDIT_XEMU_ARGS --emulator ${XEMU} --sdimg ${MEGA65_SDIMG})
            if(FREEZER_TRACE)
                list(APPEND BITEDIT_XEMU_ARGS --expect-serial)
            endif()
            add_test(NAME bitedit_xemu
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_bitedit_xemu.py
                        ${BITEDIT_XEMU_ARGS}
                        --build $<TARGET_FILE_DIR:MONITOR>)
            set_tests_properties(bitedit_xemu PROPERTIES TIMEOUT 300)

            # The same run with the database left off the card.  A missing file
            # is deliberately silent on screen, so this is the only check that
            # it degrades to an unnamed table rather than hanging or refusing.
            add_test(NAME bitedit_xemu_no_database
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_bitedit_xemu.py
                        ${BITEDIT_XEMU_ARGS} --without-iomap
                        --build $<TARGET_FILE_DIR:MONITOR>)
            set_tests_properties(bitedit_xemu_no_database PROPERTIES TIMEOUT 300)
        endif()

        # Walking a real ROM covers what synthetic opcodes cannot: our decoder
        # picks the instruction boundaries, so a wrong length lands the next
        # decode mid-instruction and the disagreement cascades.  The image is
        # not ours to redistribute, so point at your own copy; without one the
        # test simply does not exist.
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
