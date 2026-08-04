# Fails the build if a .M65 would overrun the charset at $9000.
file(SIZE "${FILE}" size)
get_filename_component(name "${FILE}" NAME)
if(size GREATER MAX_SIZE)
    message(FATAL_ERROR "${name} is ${size} bytes, over the ${MAX_SIZE} limit")
endif()
math(EXPR remaining "${MAX_SIZE} - ${size}")
message(STATUS "${name} ${size} bytes (${remaining} remaining)")
