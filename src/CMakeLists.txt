# version.c carries the git describe string, and is regenerated on every
# build: .git/HEAD is a symref, so committing on a branch never touches it,
# and no single path tracks a commit across packed refs, detached HEAD and
# worktrees.  copy_if_different limits the relink to builds where the stamp
# actually moved.
set(VERSION_C ${CMAKE_CURRENT_BINARY_DIR}/version.c)
add_custom_target(version_stamp
    COMMAND ${CMAKE_COMMAND} -DOUT=${VERSION_C}.tmp -DREPO=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VERSION_C}.tmp ${VERSION_C}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${VERSION_C}.tmp
    BYPRODUCTS ${VERSION_C}
    COMMENT "Stamping version"
)
set_source_files_properties(${VERSION_C} PROPERTIES GENERATED TRUE)

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
)
# include_directories() also feeds the ASM rule, so helper.s finds mega65.inc
# here too -- no separate CMAKE_ASM_FLAGS entry needed.
include_directories(${SRC})

# Must be a link option: -mlto-zp governs LTO codegen, and is silently ignored
# at compile time.  Moves together with freezemenu.ld's __basic_zp_end -- six
# of seven targets already use 108 of the 110 bytes that pair allows.  Cached
# because add_link_options() appends after CMAKE_EXE_LINKER_FLAGS and would
# otherwise override -DLTO_ZP from the command line.
set(LTO_ZP 106 CACHE STRING "Zero-page bytes the LTO allocator may use")
add_link_options(-mlto-zp=${LTO_ZP})

set(COMMON
    frozen_memory.c fdisk_memory.c fdisk_screen.c
    fdisk_hal_mega65.c freezer_common.c
    helper.s ${VERSION_C}
)

function(add_m65 name)
    add_executable(${name} ${ARGN} ${COMMON})
    add_dependencies(${name} version_stamp)
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
add_m65(MONITOR  monitor.c freeze_monitor.c
                 fdisk_screen_monitor.c charset.c)
add_m65(MAKEDISK makedisk.c fdisk_fat32.c charset.c)
add_m65(ROMLOAD  romload.c freeze_romload.c charset.c)
add_m65(SPRITED  sprited.c freeze_sprited.c charset.c)
add_m65(MEGAINFO megainfo.c freeze_megainfo.c charset.c cc65compat.c)
