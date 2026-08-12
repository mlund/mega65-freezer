# Regenerating files that are committed.  The first two write back into src/, so
# a normal build needs neither Python nor llvm-mc; they run after a toolchain
# bump or a mega65-core update, and their output is reviewed like any edit.
# theme-shots produces a picture for documentation instead, and is committed
# nowhere.

if(Python3_FOUND AND MEGA65_IOMAP AND EXISTS ${MEGA65_IOMAP})
    add_custom_target(iomap-names
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_iomap_names.py
                --iomap ${MEGA65_IOMAP} -o ${SRC}/monitor/iomap.bin
                --labels ${SRC}/monitor/cpumap_labels.inc
        COMMENT "Regenerating the I/O register database and map labels")
endif()

if(Python3_FOUND AND LLVM_MC_EXE)
    add_custom_target(disasm-tables
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_disasm_tables.py
                --llvm-mc ${LLVM_MC_EXE} -o ${SRC}/monitor/disasm_tables.inc
        COMMENT "Regenerating 45GS02 disassembler tables")
endif()

# What the colour schemes look like beside each other, which neither the source
# nor a screen dump can show: colour RAM holds palette entry numbers, so every
# scheme dumps identically.  Needs the emulator and a card to clone.
if(Python3_FOUND AND XEMU AND MEGA65_SDIMG AND EXISTS ${MEGA65_SDIMG})
    add_custom_target(theme-shots
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/theme-shots.py
                --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                --build $<TARGET_FILE_DIR:FREEZER>
                --workdir ${CMAKE_BINARY_DIR}/theme-shots
                -o ${CMAKE_BINARY_DIR}/themes.jpg
        DEPENDS FREEZER
        COMMENT "Photographing the freeze menu in each colour scheme")
endif()
