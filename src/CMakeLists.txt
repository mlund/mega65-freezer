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
)
# include_directories() also feeds the ASM rule, so helper.S finds helper.h
# here too -- no separate CMAKE_ASM_FLAGS entry needed.
# The SDK's MEGA65 register structs (VICIV, CIA, SID, PALETTE).  Headers only:
# the platform stays c64, because the mega65 platform emits a BASIC 65 stub
# this loader cannot enter.  Derived from the SDK package location rather than
# an absolute path, so it follows wherever llvm-mos is installed.
cmake_path(GET llvm-mos-sdk_DIR PARENT_PATH _sdk_cmake)   # .../lib/cmake
cmake_path(GET _sdk_cmake PARENT_PATH _sdk_lib)           # .../lib
cmake_path(GET _sdk_lib PARENT_PATH LLVM_MOS_ROOT)        # install prefix
set(MEGA65_INCLUDE ${LLVM_MOS_ROOT}/mos-platform/mega65/include)
if(NOT EXISTS ${MEGA65_INCLUDE}/mega65.h)
    message(FATAL_ERROR "MEGA65 register headers not found at ${MEGA65_INCLUDE}")
endif()
include_directories(${SRC} ${CMAKE_CURRENT_BINARY_DIR} ${MEGA65_INCLUDE})

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
