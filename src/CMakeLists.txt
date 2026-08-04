# version.c carries the git describe string.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/version.c
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_SOURCE_DIR}/version.c
            -DREPO=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/gitversion.sh ${GIT_HEAD}
)

# Targets are defined here; the parent CMakeLists handles project setup and
# the mega65-libc dependency.

# $9000 (charset destination) - $07ff (load address)
set(MAX_SIZE 34817)

add_compile_options(
    -Os
    # $0000/$0001 are the CPU port registers, so POKE(0,..) is a store through
    # a null pointer.  Without this, clang treats it as undefined behaviour and
    # deletes the rest of the enclosing function; main() disappears entirely.
    -fno-delete-null-pointer-checks
    -Wno-return-mismatch
)
include_directories(${SRC})

# -mlto-zp governs LTO codegen, which happens at link time; as a compile option
# it is silently ignored and the zero-page region overflows.
add_link_options(-mlto-zp=106)

# helper.s includes mega65.inc from the same directory.
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -I${SRC}")

set(COMMON
    frozen_memory.c fdisk_memory.c fdisk_screen.c
    fdisk_hal_mega65.c freezer_common.c
    helper.s version.c
)

function(add_m65 name)
    add_executable(${name} ${ARGN} ${COMMON})
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
add_m65(MEGAINFO megainfo.c freeze_megainfo.c charset.c
                 infohelper.c cc65compat.c)
