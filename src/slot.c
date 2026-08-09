#include "slot.h"

#include "common.h"
#include "dma.h"
#include "errors.h"
#include "screen.h"
#include "sdcard.h"
#include "trace.h"

#include <stdio.h>

unsigned char freeze_region_flags = 0;

uint32_t freeze_slot_start_sector = 0;

void request_freeze_region_list(void) {
    /* The hypervisor will only deliver the list below 32KB, so it lands on the
     * screen and is copied from there. */
    uint16_t i;
    fetch_freeze_region_list_from_hypervisor(0x0400U);
    lcopy(0x0400U, (uint32_t)&freeze_region_list, 256);

    freeze_region_flags = 0;
    for (i = 0; i < MAX_REGIONS; i++) {
        if (freeze_region_list[i].address_base == CHARGEN_ADDRESS) {
            freeze_region_flags |= FreezeRegionHasChargen;
        }
        if (freeze_region_list[i].freeze_prep == 0xFF) {
            break;
        }
    }
    freeze_region_count = (unsigned char)i;
}

uint32_t read_freeze_slot_start_sector(uint16_t slot) {
    find_freeze_slot_start_sector(slot);
    return *(volatile uint32_t*)0xD681U;
}

/* Each of these reads or writes a whole sector per byte: there is no cache
 * here, and a caller that touches many bytes should keep one of its own, as
 * monitor/commands.c does. */
unsigned char freeze_peek(uint32_t addr) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);
    uint16_t offset;

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        /* FreezerNotFrozen's value, but this function returns data, so a real
         * byte of $55 is indistinguishable from the failure. */
        TRACE("not in freeze slot");
        return FreezerNotFrozen;
    }

    offset = freeze_slot_offset & 0x1ff;
    freeze_slot_offset = freeze_slot_offset >> 9L;

    sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);

    return sector_buffer[offset & 0x1ff];
}

enum FreezerError freeze_fetch_sector(uint32_t addr, unsigned char* buffer) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);
    uint16_t offset;

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        TRACE("not in freeze slot");
        return FreezerNotFrozen;
    }

    offset = freeze_slot_offset & 0x1ff;
    freeze_slot_offset = freeze_slot_offset >> 9L;

    sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);

    if (buffer != NULL) {
        lcopy((long)&sector_buffer[offset], (long)buffer, 512 - offset);
    }

    return FreezerOk;
}

enum FreezerError freeze_fetch_sector_partial(uint32_t addr, uint32_t dest, uint16_t count) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);
    uint16_t offset;

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        TRACE("not in freeze slot");
        return FreezerNotFrozen;
    }

    if (count > 512) {
        TRACE("count exceeds one sector");
        return FreezerCountTooLarge;
    }
    offset = freeze_slot_offset & 0x1ff;
    freeze_slot_offset = freeze_slot_offset >> 9L;

    sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);

    lcopy((long)&sector_buffer[offset], dest, count);

    return FreezerOk;
}

enum FreezerError freeze_store_sector(uint32_t addr, unsigned char* buffer) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);
    uint16_t offset;

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        TRACE("not in freeze slot");
        return FreezerNotFrozen;
    }

    offset = freeze_slot_offset & 0x1ff;
    freeze_slot_offset = freeze_slot_offset >> 9L;

    /* A partial store has to read what it is not replacing. */
    if (offset > 0) {
        sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);
    }

    if (buffer != NULL) {
        lcopy((long)buffer,
            (long)&sector_buffer[offset],
            512 - offset); // don't write behind the buffer!
    }

    sdcard_writesector(freeze_slot_start_sector + freeze_slot_offset, 0);

    return FreezerOk;
}

enum FreezerError freeze_store_sector_partial(uint32_t addr, uint32_t src, uint16_t count) {

    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);
    uint16_t offset;

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        TRACE("not in freeze slot");
        return FreezerNotFrozen;
    }

    if (count > 512) {
        TRACE("count exceeds one sector");
        return FreezerCountTooLarge;
    }
    offset = freeze_slot_offset & 0x1ff;
    freeze_slot_offset = freeze_slot_offset >> 9L;

    /* A partial store has to read what it is not replacing. */
    if (count != 512 || offset != 0) {
        sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);
    }

    lcopy(src, (long)&sector_buffer[offset], count);

    sdcard_writesector(freeze_slot_start_sector + freeze_slot_offset, 0);

    return FreezerOk;
}

void freeze_poke(uint32_t addr, unsigned char v) {
    uint32_t freeze_slot_offset = address_to_freeze_slot_offset(addr);
    uint16_t offset;

    if (freeze_slot_offset == 0xFFFFFFFFUL) {
        return;
    }

    offset = freeze_slot_offset & 0x1ff;
    freeze_slot_offset = freeze_slot_offset >> 9L;

    sdcard_readsector(freeze_slot_start_sector + freeze_slot_offset);

    sector_buffer[offset & 0x1ff] = v;

    sdcard_writesector(freeze_slot_start_sector + freeze_slot_offset, 0);
}

/* Every I/O register shares the top half of its 28-bit address, and a call site
 * that spells the whole thing out marshals those two constant bytes into
 * imaginary registers, 4 bytes of code each.  A register number is two bytes,
 * so it rides in A and X and the prefix is added once, here.
 *
 * noinline is what makes that true: the call sites pass constants, so the
 * addition would otherwise fold back through the wrapper and inlining would
 * restore the original code exactly.  This is the opposite of colour_poke(),
 * whose argument is a runtime value and which the inliner handles unprompted. */
constexpr uint32_t FROZEN_IO_BASE = 0x0FFD0000UL;

__attribute__((noinline)) unsigned char freeze_io_peek(uint16_t reg) {
    return freeze_peek(FROZEN_IO_BASE + reg);
}

__attribute__((noinline)) void freeze_io_poke(uint16_t reg, unsigned char v) {
    freeze_poke(FROZEN_IO_BASE + reg, v);
}
