# Every test here is gated on what it alone needs, so a machine missing one
# tool loses that test rather than the suite: a run that skipped everything
# looks exactly like a run that passed.  `ctest -N` lists what registered.

# --- Host tests: the pure code compiled for the host, checked against an
# oracle that is not this code's own arithmetic. -------------------------------

if(Python3_FOUND)
    add_test(NAME thumbnail_frames
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_thumbnail_frames.py)
endif()

if(Python3_FOUND AND HOST_C_COMPILER)
    # mtools is the oracle: our FAT32 writer is checked against mdir's reading
    # of the image it produced.
    find_program(MTOOLS_MDIR NAMES mdir)
    if(MTOOLS_MDIR AND MEGA65_SDIMG AND EXISTS ${MEGA65_SDIMG})
        add_test(NAME fat32
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_fat32.py
                    --sdimg ${MEGA65_SDIMG})
        set_tests_properties(fat32 PROPERTIES TIMEOUT 300)
    endif()

    add_test(NAME format
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_format.py
                --cc ${HOST_C_COMPILER})

    # Disk geometry, FAT records and RTC decoding against what hyppo and the
    # 45IO27 registers require.  A wrong image size mounts and then reads back
    # the wrong half.
    add_test(NAME layout
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_layout.py
                --cc ${HOST_C_COMPILER})

    # Which sector of the freeze slot an address falls in, against hyppo's own
    # freeze_mem_list.  A byte costs a whole sector transfer, so this decides
    # both correctness and how much SD traffic a routine spends.
    add_test(NAME slotmap
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_slotmap.py
                --cc ${HOST_C_COMPILER})

    # The bit editor's record stream is variable length, so an off-by-one in
    # the walk puts every name on the wrong bit -- which on hardware reads as a
    # bad database rather than a bad decoder.  MEGA65_IOMAP adds the name diff.
    set(BITEDIT_ARGS --cc ${HOST_C_COMPILER})
    if(MEGA65_IOMAP AND EXISTS ${MEGA65_IOMAP})
        list(APPEND BITEDIT_ARGS --iomap ${MEGA65_IOMAP})
    endif()
    add_test(NAME bitedit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_bitedit.py
                ${BITEDIT_ARGS})

    # blockmove.c takes its two memory accesses from the caller, so the host
    # build drives it against a flat array.  The overlap cases are the point: a
    # forward copy into a destination inside the source range corrupts only the
    # tail, which reads as working on hardware.
    add_test(NAME blockmove
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_blockmove.py
                --cc ${HOST_C_COMPILER})
endif()

# --- Disassembler: every opcode and prefix diffed against llvm-mc, so a
# toolchain bump cannot silently change the committed tables. ------------------

if(Python3_FOUND AND HOST_C_COMPILER AND LLVM_MC_EXE)
    add_test(NAME disasm
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_disasm.py
                --llvm-mc ${LLVM_MC_EXE} --cc ${HOST_C_COMPILER})

    # Walking a real ROM covers what synthetic opcodes cannot: our decoder
    # picks the instruction boundaries, so a wrong length lands the next decode
    # mid-instruction and the disagreement cascades.  The image is not ours to
    # redistribute, so point at your own copy.
    set(MEGA65_ROM "$ENV{MEGA65_ROM}" CACHE FILEPATH
        "MEGA65.ROM image for the disassembler's real-code test")
    if(MEGA65_ROM AND EXISTS ${MEGA65_ROM})
        add_test(NAME disasm_rom
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_disasm.py
                    --llvm-mc ${LLVM_MC_EXE} --cc ${HOST_C_COMPILER}
                    --rom ${MEGA65_ROM})
    endif()
endif()

# --- Emulator: self-checking PRGs, which quit with the failing check's number
# so the exit status is the verdict.  `-testing` is what makes that $D6CF write
# work.  Timeouts turn a hang into a reported failure. -------------------------

if(XEMU AND TARGET test_printf)
    add_test(NAME printf
        COMMAND ${XEMU} -headless -sleepless -testing -model 3 -hyperserialascii
                -besure -prg $<TARGET_FILE:test_printf>)
    set_tests_properties(printf PROPERTIES TIMEOUT 15)
endif()

if(XEMU AND TARGET test_dma)
    add_test(NAME dma
        COMMAND ${XEMU} -headless -sleepless -testing -model 3 -hyperserialascii
                -besure -prg $<TARGET_FILE:test_dma>)
    set_tests_properties(dma PROPERTIES TIMEOUT 60)
endif()

# --- Emulator, from the card: a tool dropped into memory cold never starts, so
# these boot the freezer off a clone of a real image carrying our builds. ------

if(XEMU AND Python3_FOUND AND MEGA65_SDIMG AND EXISTS ${MEGA65_SDIMG})
    if(TARGET test_freezeslot)
        # The freeze slot's round trip on emulated SD hardware, including the
        # whole-sector pair batching uses.  Writes, hence the clone.
        add_test(NAME freezeslot
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_freezeslot.py
                    --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                    --prg $<TARGET_FILE:test_freezeslot>)
        set_tests_properties(freezeslot PROPERTIES TIMEOUT 180)
    endif()

    # The launch path -- region list, find, load, run -- which `-prg` cannot
    # reach.
    add_test(NAME launch_monitor
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_launch.py
                --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                --build $<TARGET_FILE_DIR:FREEZER>)
    set_tests_properties(launch_monitor PROPERTIES TIMEOUT 180)

    add_test(NAME launch_audiomix
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_audiomix_xemu.py
                --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                --build $<TARGET_FILE_DIR:AUDIOMIX>)
    set_tests_properties(launch_audiomix PROPERTIES TIMEOUT 180)

    # SPRITED draws through mega65-libc's conio rather than screen.c, at its
    # own geometry and its own charset, and had no cover at all.  This pins
    # what it renders so a move onto the shared drawing code can be shown not
    # to change it.
    # The help screen sits behind a key the synthetic keyboard cannot send, so
    # those steps only run against a build that accepts F1 in its place.
    set(SPRITED_ARGS --emulator ${XEMU} --sdimg ${MEGA65_SDIMG})
    if(HARNESS_KEYS)
        list(APPEND SPRITED_ARGS --harness-keys)
    endif()
    add_test(NAME sprited
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_sprited_xemu.py
                ${SPRITED_ARGS} --build $<TARGET_FILE_DIR:SPRITED>)
    set_tests_properties(sprited PROPERTIES TIMEOUT 300)

    # The freeze menu's own screen, which the tests above only pass through:
    # that it draws, and that a key redraws the field it owns.
    add_test(NAME freezer_menu
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_freezer_xemu.py
                --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                --build $<TARGET_FILE_DIR:FREEZER>)
    set_tests_properties(freezer_menu PROPERTIES TIMEOUT 300)

    # The chooser's list, MAKEDISK's dialog and its FAT32 writer in one run:
    # create an image the way a user does and read it back off the clone.  The
    # only cover diskchooser.c and MAKEDISK have.
    add_test(NAME makedisk_create
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_makedisk_xemu.py
                --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                --build $<TARGET_FILE_DIR:MAKEDISK>)
    set_tests_properties(makedisk_create PROPERTIES TIMEOUT 600)

    # What creating a D81 costs the card, in transactions rather than seconds:
    # the emulator has no card to be slow.  Only with the counters built in.
    if(SDCARD_COUNTERS)
        add_test(NAME sdcount
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_sdcount_xemu.py
                    --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                    --build $<TARGET_FILE_DIR:MAKEDISK>)
        set_tests_properties(sdcount PROPERTIES TIMEOUT 600)
    endif()

    # Assemble in the monitor and read the bytes back off the card.
    add_test(NAME monitor_assemble
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_monitor.py
                --emulator ${XEMU} --sdimg ${MEGA65_SDIMG}
                --build $<TARGET_FILE_DIR:MONITOR>)
    set_tests_properties(monitor_assemble PROPERTIES TIMEOUT 300)

    # The same round trip for `B`: toggle a named bit, type a value over it,
    # leave, read elsewhere so the sector is displaced, and reopen -- so the
    # byte that comes back has been off the card.  FREEZER_TRACE additionally
    # requires the write log on the hypervisor serial channel.
    set(BITEDIT_XEMU_ARGS --emulator ${XEMU} --sdimg ${MEGA65_SDIMG})
    if(FREEZER_TRACE)
        list(APPEND BITEDIT_XEMU_ARGS --expect-serial)
    endif()
    add_test(NAME bitedit_xemu
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_bitedit_xemu.py
                ${BITEDIT_XEMU_ARGS} --build $<TARGET_FILE_DIR:MONITOR>)
    set_tests_properties(bitedit_xemu PROPERTIES TIMEOUT 300)

    # The same run with the database left off the card.  A missing file is
    # deliberately silent on screen, so this is the only check that it degrades
    # to an unnamed table rather than hanging or refusing.
    add_test(NAME bitedit_xemu_no_database
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_bitedit_xemu.py
                ${BITEDIT_XEMU_ARGS} --without-iomap
                --build $<TARGET_FILE_DIR:MONITOR>)
    set_tests_properties(bitedit_xemu_no_database PROPERTIES TIMEOUT 300)
endif()
