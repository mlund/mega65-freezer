# Regenerating files that are committed.  Both write back into src/, so a
# normal build needs neither Python nor llvm-mc; these run after a toolchain
# bump or a mega65-core update, and their output is reviewed like any edit.

if(Python3_FOUND AND MEGA65_IOMAP AND EXISTS ${MEGA65_IOMAP})
    add_custom_target(iomap-names
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_iomap_names.py
                --iomap ${MEGA65_IOMAP} -o ${SRC}/monitor/iomap.bin
        COMMENT "Regenerating the I/O register database")
endif()

if(Python3_FOUND AND LLVM_MC_EXE)
    add_custom_target(disasm-tables
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_disasm_tables.py
                --llvm-mc ${LLVM_MC_EXE} -o ${SRC}/monitor/disasm_tables.inc
        COMMENT "Regenerating 45GS02 disassembler tables")
endif()
