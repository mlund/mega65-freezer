# Where the emulator tests get their card.
#
# The freezer loads its tools from the card, and Xemu's own image carries stock
# MEGA65 binaries, so a test run without one would exercise upstream's tools
# rather than ours.  The image is cloned before our builds are copied in, so the
# one named here is never written to.  None is committed: such an image contains
# MEGA65.ROM.

set(MEGA65_SDIMG "$ENV{MEGA65_SDIMG}" CACHE FILEPATH
    "SD image to clone for the emulator tests, e.g. Xemu's own mega65.img")

# Fall back to Xemu's own card, which the emulator search implies is there.
# Worth doing because the failure is silent: with no image the emulator tests do
# not register at all, and a run that skipped them looks like a run that passed.
if(NOT MEGA65_SDIMG)
    foreach(candidate
            "$ENV{HOME}/Library/Application Support/xemu-lgb/mega65/mega65.img"
            "$ENV{HOME}/.local/share/xemu-lgb/mega65/mega65.img")
        if(EXISTS "${candidate}")
            set(MEGA65_SDIMG "${candidate}" CACHE FILEPATH
                "SD image to clone for the emulator tests" FORCE)
            message(STATUS "SD image for the emulator tests: ${MEGA65_SDIMG}")
            break()
        endif()
    endforeach()
endif()
