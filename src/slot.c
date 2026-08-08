#include "slot.h"

#include "common.h"
#include "dma.h"
#include "errors.h"
#include "screen.h"
#include "sdcard.h"
#include "trace.h"

#include <stdio.h>

struct FreezeRegion freeze_region_list[MAX_REGIONS];
unsigned char freeze_region_count = 0;
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

uint32_t find_thumbnail_offset(void) {
    uint32_t freeze_slot_offset =
        1; // Skip the initial saved SD sector at the beginning of each slot
    uint32_t region_length = 0;

    for (char i = 0; i < freeze_region_count; i++) {
        region_length = freeze_region_list[i].region_length & REGION_LENGTH_MASK;
        /* Two addresses because a bitstream either maps the thumbnail directly
         * at $FFD4000 or relocates it to $1000 before freezing. */
        if (freeze_region_list[i].address_base == 0x1000L ||
            freeze_region_list[i].address_base == 0xffd4000L) {
            return freeze_slot_offset;
        }

        freeze_slot_offset += region_length >> 9;
        /* A part-used sector still occupies a whole one. */
        if (region_length & 0x1ff) {
            freeze_slot_offset++;
        }
    }
    return 0xFFFFFFFFUL;
}

/* Convert a requested address to a location in the freeze slot,
   or to 0xFFFFFFFF if the address is not present.
*/
uint32_t address_to_freeze_slot_offset(uint32_t address) {
    uint32_t freeze_slot_offset =
        1; // Skip the initial saved SD sector at the beginning of each slot
    uint32_t relative_address = 0;
    uint32_t region_length = 0;
    char skip;
    char i;

    for (i = 0; i < freeze_region_count; i++) {
        skip = 0;
        if (address < freeze_region_list[i].address_base) {
            skip = 1;
        }
        relative_address = address - freeze_region_list[i].address_base;
        if (freeze_region_list[i].address_base == 0x1000L) {
            // Thumbnail region: Treat specially so that we can examine it
            // We give the fictional mapping of $FF54xxx
            if ((address & 0xFFFF000L) == 0xFF54000L) {
                relative_address = address & 0xFFF;
                freeze_slot_offset = freeze_slot_offset << 9;
                freeze_slot_offset += (relative_address & 0xFFF);
                return freeze_slot_offset;
            }
        }
        region_length = freeze_region_list[i].region_length & REGION_LENGTH_MASK;
        if (relative_address >= region_length) {
            skip = 1;
        }
        if (skip) {
            // Skip this region if our address is not in it
            freeze_slot_offset += region_length >> 9;
            // If region is not an integer number of sectors long, don't forget to count the partial
            // sector
            if (region_length & 0x1ff) {
                freeze_slot_offset++;
            }
        } else {
            // The address is in this region.

            // Firsts add the number of sectors to get to the one with the content we want
            freeze_slot_offset += relative_address >> 9;

            // Now multiply it by the length of a sector (512 bytes), and add the offset in the
            // sector This gives us the absolute byte position in the slot of the address we want.
            freeze_slot_offset = freeze_slot_offset << 9;
            freeze_slot_offset += (relative_address & 0x1FF);
            return freeze_slot_offset;
        }
    }
    return 0xFFFFFFFFUL;
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
