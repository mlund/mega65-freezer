# Large arrays below the region top cost C budget without showing in the .M65
# size, so checksize.cmake cannot see them: .bss occupies the same
# $0801-$9000 span as the code but never reaches the file.
#
# Placement is what matters, not size.  The same array above the top costs
# nothing, which is what src/link.ld is for -- a 4KB thumbnail buffer up
# there is free, and half a kilobyte down here is half a kilobyte the C cannot
# have.
#
# Adding a name to ALLOW is a decision that it has to live in the region.
# Moving it to memory.ld is the alternative, and usually the better one.

execute_process(COMMAND ${NM} --print-size --defined-only ${FILE}
    OUTPUT_VARIABLE symbols RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "could not read symbols from ${FILE}")
endif()

get_filename_component(name "${FILE}" NAME)
string(REPLACE "," ";" ALLOW "${ALLOW}")
string(REPLACE "\n" ";" lines "${symbols}")

set(offenders "")
foreach(line IN LISTS lines)
    # "<address> <size> <type> <name>"; types b and d are the writable ones.
    if(line MATCHES "^([0-9a-fA-F]+) +([0-9a-fA-F]+) +([bBdD]) +(.+)$")
        math(EXPR address "0x${CMAKE_MATCH_1}")
        math(EXPR size "0x${CMAKE_MATCH_2}")
        set(symbol "${CMAKE_MATCH_4}")
        if(address LESS REGION_TOP AND NOT size LESS THRESHOLD)
            list(FIND ALLOW "${symbol}" allowed)
            if(allowed EQUAL -1)
                math(EXPR at "${address}" OUTPUT_FORMAT HEXADECIMAL)
                list(APPEND offenders "${symbol}, ${size} bytes at ${at}")
            endif()
        endif()
    endif()
endforeach()

if(offenders)
    string(REPLACE ";" "\n    " report "${offenders}")
    message(FATAL_ERROR
        "${name}: these sit in the region the C code shares and are not listed "
        "as deliberate:\n    ${report}\n"
        "Place them in src/link.ld, or add the name to LOWMEM_ALLOW in "
        "src/CMakeLists.txt to say they belong here.")
endif()
