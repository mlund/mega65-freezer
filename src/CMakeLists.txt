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

# Targets are defined here; the parent CMakeLists handles project setup and
# the mega65-libc dependency.

# $9000 (charset destination) - $07ff (load address)
set(MAX_SIZE 34817)

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
# include_directories() also feeds the ASM rule, so helper.S finds helper.h
# here too -- no separate CMAKE_ASM_FLAGS entry needed.
# The SDK's MEGA65 register structs (VICIV, CIA, SID, PALETTE).  Headers only:
# the platform stays c64, because the mega65 platform emits a BASIC 65 stub
# this loader cannot enter.  LLVM_MOS_ROOT comes from the parent scope.
set(MEGA65_INCLUDE ${LLVM_MOS_ROOT}/mos-platform/mega65/include)
if(NOT EXISTS ${MEGA65_INCLUDE}/mega65.h)
    message(FATAL_ERROR "MEGA65 register headers not found at ${MEGA65_INCLUDE}")
endif()
include_directories(${SRC} ${CMAKE_CURRENT_BINARY_DIR})
# SYSTEM: the SDK headers are not ours to fix, and <mega65.h> disables
# -Wfixed-enum-extension, a warning this clang does not know.  Marking them
# system suppresses that at the source rather than switching the diagnostic off
# for our own code too.
include_directories(SYSTEM ${MEGA65_INCLUDE})

# Must be a link option: -mlto-zp governs LTO codegen, and is silently ignored
# at compile time.  Moves together with freezemenu.ld's __basic_zp_end; the
# allocator saturates whatever the pair allows, so the two are one setting.
# Cached because add_link_options() appends after CMAKE_EXE_LINKER_FLAGS and
# would otherwise override -DLTO_ZP from the command line.
set(LTO_ZP 210 CACHE STRING "Zero-page bytes the LTO allocator may use")
add_link_options(-mlto-zp=${LTO_ZP} -Wl,--gc-sections)

set(COMMON
    frozen_memory.c fdisk_memory.c fdisk_screen.c
    fdisk_hal_mega65.c freezer_common.c
    helper.S version.c
)

function(add_m65 name)
    add_executable(${name} ${ARGN} ${COMMON})
    add_dependencies(${name} version_stamp)
    # See C_MCPU in the top-level CMakeLists.  Per target so that lowering it
    # leaves mega65-libc alone; C only, as the ASM rule keeps mos45gs02.
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:-mcpu=${C_MCPU}>)
    if(LINK_LIBC)
        target_link_libraries(${name} mega65libc)
    endif()
    target_link_options(${name} PRIVATE -T ${CMAKE_CURRENT_SOURCE_DIR}/freezemenu.ld)
    set_target_properties(${name} PROPERTIES OUTPUT_NAME ${name}.M65)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DFILE=$<TARGET_FILE:${name}> -DMAX_SIZE=${MAX_SIZE}
                -P ${CMAKE_SOURCE_DIR}/cmake/checksize.cmake)
endfunction()

add_m65(FREEZER  freezer.c freeze_diskchooser.c)
add_m65(AUDIOMIX audiomix.c freeze_audiomix.c charset.c)
add_m65(MONITOR  monitor/main.c monitor/freeze_monitor.c monitor/disasm.c
                 monitor/blockmove.c monitor/fdisk_screen_monitor.c charset.c)
target_include_directories(MONITOR PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/monitor)
add_m65(MAKEDISK makedisk.c fdisk_fat32.c charset.c)
add_m65(ROMLOAD  romload.c freeze_romload.c charset.c)
add_m65(SPRITED  sprited.c freeze_sprited.c charset.c)
add_m65(MEGAINFO megainfo.c freeze_megainfo.c charset.c cc65compat.c)
