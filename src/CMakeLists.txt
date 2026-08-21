# Targets are defined here; the parent CMakeLists handles project setup and the
# mega65-libc dependency.

# version.h carries the git describe string, and is regenerated on every
# build: .git/HEAD is a symref, so committing on a branch never touches it,
# and no single path tracks a commit across packed refs, detached HEAD and
# worktrees.  copy_if_different limits the relink to builds where the stamp
# actually moved.
set(VERSION_H ${CMAKE_CURRENT_BINARY_DIR}/version.h)
add_custom_target(version_stamp
    COMMAND ${CMAKE_COMMAND} -DOUT=${VERSION_H} -DREPO=${CMAKE_SOURCE_DIR}
            -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
            -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${VERSION_H}
    COMMENT "Stamping version"
)
set_source_files_properties(${VERSION_H} PROPERTIES GENERATED TRUE)

# $9000 (the charset shadow, which a tool must not run into) - $07ff (load address)
set(MAX_SIZE 34817)

# The same $9000, as an address: everything below it is shared between the code
# and its data, so a large array there is budget the C cannot have.  See
# cmake/checkbuffers.cmake, and src/lowmem.ld for where such an array can go
# instead.  Each name below is a decision that it stays; none has been examined
# closely, so they are candidates rather than conclusions.
set(REGION_TOP 36864)
set(LOWMEM_THRESHOLD 256)
set(LOWMEM_ALLOW
    sector_buffer       # every sector read and written goes through it
    freeze_region_list  # hyppo's table, read at startup and kept
    process_descriptor
    colour_table
    mon_sector
    block_buffer
    code_buffer
    buffer
    escapeCode
    rom_reset_screen
)
# Commas, not semicolons: a CMake list on a command line splits into separate
# arguments.
string(REPLACE ";" "," LOWMEM_ALLOW_ARG "${LOWMEM_ALLOW}")
find_program(LLVM_NM_EXE NAMES llvm-nm HINTS ${LLVM_MOS_ROOT}/bin NO_DEFAULT_PATH)

# Hypervisor serial tracing on the failure paths.  Off by default: a shipping
# build carries no strings for failures that do not happen.  See src/trace.h.
option(FREEZER_TRACE "Trace failure paths over the hypervisor serial channel" OFF)
if(FREEZER_TRACE)
    add_compile_definitions(FREEZER_TRACE)
endif()

# Ethernet diagnostics.  Off by default so a shipping build carries neither
# the counters nor the increments; on, the hardware scenario reads them by
# name to separate controller, transport and transmit failures.
option(ETH_COUNTERS "Count Ethernet and transport diagnostic events" OFF)
if(ETH_COUNTERS)
    add_compile_definitions(ETH_COUNTERS)
endif()

# FILEHOST's E and R keys, which ARP a fixed address and report what came back.
# Off by default: it settled the question it was written for -- the controller
# does work from a frozen machine -- and a kilobyte of code and buffers is a
# great deal to carry for asking again.  The addresses it probes are compiled
# in, so a different LAN needs a rebuild whatever this is set to.
option(ETH_PROBE "FILEHOST's E and R ethernet probe keys" OFF)
if(ETH_PROBE)
    add_compile_definitions(ETH_PROBE)
endif()

# A card that will not come ready, on demand.  Off by default: this exists so a
# scenario can prove the write path returns rather than spins, which no
# emulator can stage on its own -- Xemu's card is never busy.  See
# test/verify_sdfault_xemu.py.
option(SDCARD_FAULTS "Let a test refuse the SD card's ready check" OFF)
if(SDCARD_FAULTS)
    add_compile_definitions(SDCARD_FAULTS)
endif()

option(SDCARD_COUNTERS "Count SD card reads and writes" OFF)
if(SDCARD_COUNTERS)
    add_compile_definitions(SDCARD_COUNTERS)
endif()

# Keys a test can reach standing in for ones it cannot.  The synthetic keyboard
# does not manage HELP -- test/xemu_keys.py says why, and why it is not
# understood -- so a screen behind it is untestable without this.  Off by
# default: the shipping tools answer only the keys they document.
option(HARNESS_KEYS "Accept substitute keys the test harness can send" OFF)
if(HARNESS_KEYS)
    add_compile_definitions(HARNESS_KEYS)
endif()

add_compile_options(
    # -Oz over -Os: smaller on every target, and nothing here is speed-critical.
    -Oz
    # $0000/$0001 are the CPU port registers, so POKE(0,..) is a legitimate
    # store through a null pointer rather than undefined behaviour.
    -fno-delete-null-pointer-checks
    # Scoped to this directory so mega65-libc, which is not ours to fix, stays
    # quiet.  These found two shipped bugs the day they were switched on, so
    # they are errors: a warning nobody has to act on is one everybody scrolls
    # past, and that is how report_unmapped() shipped calling itself.
    -Werror
    -Wall
    -Wextra
    -Wconditional-uninitialized
    # The narrowing half only.  -Wsign-conversion is the other 226 findings and
    # is the same promotion noise as bugprone-signed-bitwise: C promotes every
    # uint8_t operand to signed int, so ordinary register code fires.
    -Wconversion
    -Wno-sign-conversion
    # char is unsigned on llvm-mos -- verified with a _Static_assert -- so a
    # char subscript is 0..255 and cannot go negative.  The warning guards
    # against a signed-char platform this one is not.
    -Wno-char-subscripts
)
# The SDK's MEGA65 register structs (VICIV, CIA, SID, PALETTE) and its KERNAL
# and hypervisor wrappers.  The platform stays c64, because the mega65 platform
# emits a BASIC 65 stub this loader cannot enter, so the archive holding those
# wrappers has to be named here: the c64 driver does not put it on the link
# line.  Archive, not object, so a tool pays only for what it calls.
# LLVM_MOS_ROOT comes from the parent scope.
set(MEGA65_INCLUDE ${LLVM_MOS_ROOT}/mos-platform/mega65/include)
set(MEGA65_LIBC ${LLVM_MOS_ROOT}/mos-platform/mega65/lib/libc.a)
if(NOT EXISTS ${MEGA65_INCLUDE}/mega65.h)
    message(FATAL_ERROR "MEGA65 register headers not found at ${MEGA65_INCLUDE}")
endif()
if(NOT EXISTS ${MEGA65_LIBC})
    message(FATAL_ERROR "MEGA65 platform libc not found at ${MEGA65_LIBC}")
endif()
# An SDK too old to carry the wrappers has the archive all the same, so its
# presence says nothing: the failure would be an undefined symbol at the first
# call site, naming the wrapper rather than the reason.  The header is what is
# always readable -- a check needing a tool is a check that can be skipped --
# and it ships with the archive.  setname_page is the newest of the wrappers
# this tree calls, so it is the one that dates the SDK.
file(READ ${MEGA65_INCLUDE}/mega65.h MEGA65_HEADER)
if(NOT MEGA65_HEADER MATCHES "mega65_h_setname_page")
    message(FATAL_ERROR
        "The MEGA65 SDK at ${LLVM_MOS_ROOT} predates the hyppo and kernal "
        "wrappers.  Point CMAKE_PREFIX_PATH at one that has them.")
endif()
unset(MEGA65_HEADER)

# The wrappers, and the platform libc that must be searched before them: the
# link line puts these ahead of the driver's own, so naming c64's first is what
# keeps the platform choice at the top of this tree meaningful.  Without it the
# whole C library silently comes from the mega65 archive instead.
add_library(mega65sdk INTERFACE)
target_link_libraries(mega65sdk INTERFACE
    ${LLVM_MOS_ROOT}/mos-platform/c64/lib/libc.a ${MEGA65_LIBC})
# Also feeds the ASM rule, so helper.S finds helper.h here too -- no separate
# CMAKE_ASM_FLAGS entry needed.
include_directories(${SRC} ${CMAKE_CURRENT_BINARY_DIR})
# SYSTEM: the SDK headers are not ours to fix, and <mega65.h> disables
# -Wfixed-enum-extension, a warning this clang does not know.  Marking them
# system suppresses that at the source rather than switching the diagnostic off
# for our own code too.
include_directories(SYSTEM ${MEGA65_INCLUDE})

# Must be a link option: -mlto-zp governs LTO codegen, and is silently ignored
# at compile time.  Moves together with link.ld's __basic_zp_end; the
# allocator saturates whatever the pair allows, so the two are one setting.
# Cached because add_link_options() appends after CMAKE_EXE_LINKER_FLAGS and
# would otherwise override -DLTO_ZP from the command line.
set(LTO_ZP 210 CACHE STRING "Zero-page bytes the LTO allocator may use")
add_link_options(-mlto-zp=${LTO_ZP} -Wl,--gc-sections)

set(COMMON
    slot.c slotmap.c dma.c screen.c format.c screencode.c
    sdcard.c common.c hyppo.c
    helper.S version.c
)

function(add_m65 name)
    add_executable(${name} ${ARGN} ${COMMON})
    add_dependencies(${name} version_stamp)
    # See C_MCPU in the top-level CMakeLists.  Per target so that lowering it
    # leaves mega65-libc alone; C only, as the ASM rule keeps mos45gs02.
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${C_MCPU}>)
    if(LINK_LIBC)
        target_link_libraries(${name} mega65libc)
    endif()
    target_link_libraries(${name} mega65sdk)
    # -L so that link.ld's INCLUDE of lowmem.ld resolves from the build
    # directory.
    target_link_options(${name} PRIVATE -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
        -Wl,-L,${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME ${name}.M65)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DFILE=$<TARGET_FILE:${name}> -DMAX_SIZE=${MAX_SIZE}
                -P ${CMAKE_SOURCE_DIR}/cmake/checksize.cmake)
    if(LLVM_NM_EXE)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DFILE=$<TARGET_FILE:${name}>.elf -DNM=${LLVM_NM_EXE}
                    -DTHRESHOLD=${LOWMEM_THRESHOLD} -DREGION_TOP=${REGION_TOP}
                    -DALLOW=${LOWMEM_ALLOW_ARG}
                    -P ${CMAKE_SOURCE_DIR}/cmake/checkbuffers.cmake)
    endif()
endfunction()

add_m65(FREEZER  freezer/main.c freezer/diskchooser.c freezer/menu.cpp)
add_m65(AUDIOMIX audiomix/main.c audiomix/mixer.c audiomix/menu.cpp)
add_m65(MONITOR  monitor/main.c monitor/commands.c monitor/disasm.c
                 monitor/blockmove.c monitor/console.c monitor/bitedit.c
                 monitor/bitedit_table.c monitor/cpumap.c)
target_include_directories(MONITOR PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/monitor)
add_m65(MAKEDISK makedisk/main.c fat32.c layout.c)
add_m65(ROMLOAD  romload/main.c romload/chooser.c)
add_m65(SPRITED  sprited/main.c sprited/editor.c sprited/textout.c lineedit.c)
add_m65(MEGAINFO megainfo/main.c megainfo/info.c layout.c)
add_m65(FILEHOST filehost/main.c filehost/catalog.c filehost/shortname.c
                 filehost/eth.c filehost/arp.c filehost/ip.c filehost/dhcp.c
                 filehost/netpoll.c filehost/tcp.c filehost/http.c filehost/jsoncat.c
                 filehost/fetch.c filehost/view.c filehost/prgload.c
                 lineedit.c fat32.c layout.c filehost/menu.cpp)

# Card payloads: not linked into any tool, but built here because they are
# loaded off the card at run time and staged from this directory.
add_custom_target(stage-iomap ALL
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/IOMAP.M65
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/monitor/iomap.bin
            ${CMAKE_CURRENT_BINARY_DIR}/IOMAP.M65
    COMMENT "Staging IOMAP.M65")

# The frames drawn around a slot's thumbnail, picked by machine type.
# Generated rather than committed: the generator is plain Python, so unlike
# IOMAP.M65 there is nothing to go missing.
if(Python3_FOUND)
    set(THUMB_FRAMES)
    foreach(machine m65 c65 c64)
        string(TOUPPER ${machine} MACHINE)
        set(frame ${CMAKE_CURRENT_BINARY_DIR}/${MACHINE}THUMB.M65)
        add_custom_command(OUTPUT ${frame}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tools/gen_thumbnail_frames.py
                    ${CMAKE_SOURCE_DIR}/assets/thumbnail-surround-${machine}.png
                    --xoff 5 --yoff 1 -o ${frame}
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/thumbnail-surround-${machine}.png
                    ${CMAKE_SOURCE_DIR}/tools/gen_thumbnail_frames.py
            COMMENT "Generating ${MACHINE}THUMB.M65")
        list(APPEND THUMB_FRAMES ${frame})
    endforeach()
    add_custom_target(thumbnail-frames ALL DEPENDS ${THUMB_FRAMES})
endif()

# Bytes per character cell, and so the DMA destination step for both planes.
# SCREEN_ROW_BYTES is 80 in every tool and means two different things; this is
# the number that tells them apart.  See src/screen.h.
foreach(t FREEZER AUDIOMIX MAKEDISK ROMLOAD)
    target_compile_definitions(${t} PRIVATE SCREEN_CELL_BYTES=2)
endforeach()
foreach(t MONITOR MEGAINFO SPRITED FILEHOST)
    target_compile_definitions(${t} PRIVATE SCREEN_CELL_BYTES=1)
endforeach()

# Xemu test targets.  Plain c64 PRGs, not .M65: they are launched with Xemu's
# `-prg`, so they want the platform's own BASIC stub rather than link.ld's
# $080D trampoline.  mega65-libc supplies assert_eq/xemu_exit/debug_msg, so the
# tests only build when it is linked.
if(LINK_LIBC)
    add_executable(test_dma ${CMAKE_SOURCE_DIR}/test/test_dma.c dma.c)
    target_compile_options(test_dma PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${C_MCPU}>)
    target_link_libraries(test_dma mega65libc mega65sdk)
    set_target_properties(test_dma PROPERTIES OUTPUT_NAME test_dma.prg)

    # The freeze slot's round trip: a byte at a time, and the whole-sector pair
    # that batching uses instead.  Needs COMMON for the slot and SD code, and a
    # card at run time, so CTest gives it a cloned image.
    add_executable(test_freezeslot ${CMAKE_SOURCE_DIR}/test/test_freezeslot.c ${COMMON})
    add_dependencies(test_freezeslot version_stamp)
    target_compile_options(test_freezeslot PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${C_MCPU}>)
    target_compile_definitions(test_freezeslot PRIVATE SCREEN_CELL_BYTES=2)
    target_link_libraries(test_freezeslot mega65libc mega65sdk)
    # The c64 platform script reserves less zero page than link.ld does, so the
    # project-wide -mlto-zp does not fit here.  lowmem.ld comes along because
    # slot.c names hyppo's page; as an input file rather than -T, which would
    # replace the platform's script instead of adding to it.
    target_link_options(test_freezeslot PRIVATE -mlto-zp=16
        ${CMAKE_CURRENT_SOURCE_DIR}/lowmem.ld
        ${CMAKE_CURRENT_SOURCE_DIR}/test_freezeslot.ld)
    set_target_properties(test_freezeslot PROPERTIES OUTPUT_NAME test_freezeslot.prg)

    # What the HTTP transport does on a real wire, with no browser in the way.
    # A plain PRG so etherload can push it and run it: measuring through
    # FILEHOST measures its screen and its card writes too, and a serial
    # monitor watching the status line halts the CPU at instruction boundaries
    # and slows the very transfer being timed.
    #
    # Its own ETH_COUNTERS, since the numbers are the whole point of it and a
    # shipping FILEHOST does not carry them.  Not a CTest entry: there is no
    # ethernet under Xemu, so this is run by hand against hardware.
    add_executable(httpbench ${CMAKE_SOURCE_DIR}/test/httpbench.c
        filehost/eth.c filehost/arp.c filehost/ip.c filehost/dhcp.c filehost/netpoll.c
        filehost/tcp.c filehost/http.c filehost/jsoncat.c filehost/fetch.c
        filehost/catalog.c ${COMMON})
    add_dependencies(httpbench version_stamp)
    target_compile_options(httpbench PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${C_MCPU}>)
    target_compile_definitions(httpbench PRIVATE SCREEN_CELL_BYTES=2 ETH_COUNTERS)
    target_include_directories(httpbench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/filehost)
    target_link_libraries(httpbench mega65libc mega65sdk)
    target_link_options(httpbench PRIVATE -mlto-zp=16
        ${CMAKE_CURRENT_SOURCE_DIR}/lowmem.ld
        ${CMAKE_CURRENT_SOURCE_DIR}/test_freezeslot.ld)
    set_target_properties(httpbench PROPERTIES OUTPUT_NAME httpbench.prg)


    # What a multi-sector write costs against one at a time.  Also run by hand
    # against hardware -- the card routines' CMD25 calls have never executed.
    # See test/sdbatch.c.
    add_executable(sdbatch ${CMAKE_SOURCE_DIR}/test/sdbatch.c fat32.c layout.c ${COMMON})
    add_dependencies(sdbatch version_stamp)
    target_compile_options(sdbatch PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${C_MCPU}>)
    target_compile_definitions(sdbatch PRIVATE SCREEN_CELL_BYTES=2)
    target_include_directories(sdbatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/filehost)
    target_link_libraries(sdbatch mega65libc mega65sdk)
    target_link_options(sdbatch PRIVATE -mlto-zp=16
        ${CMAKE_CURRENT_SOURCE_DIR}/lowmem.ld
        ${CMAKE_CURRENT_SOURCE_DIR}/test_freezeslot.ld)
    set_target_properties(sdbatch PROPERTIES OUTPUT_NAME sdbatch.prg)

    # Decimal conversion has no caller in the seven tools -- they format hex and
    # hand-roll their decimals -- so this path reaches the machine only here.
    add_executable(test_printf ${CMAKE_SOURCE_DIR}/test/test_printf.c)
    target_compile_options(test_printf PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${C_MCPU}>)
    target_link_libraries(test_printf mega65libc mega65sdk)
    set_target_properties(test_printf PROPERTIES OUTPUT_NAME test_printf.prg)
endif()
