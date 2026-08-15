/* Walking the region table: how much of the slot each region takes, and which
 * of them an address falls in. */

#include "slotmap.h"

struct FreezeRegion freeze_region_list[MAX_REGIONS];
unsigned char freeze_region_count = 0;

/* Regions are laid down in order, each occupying whole sectors, so the offset
 * of one is the sum of those before it. */
static uint32_t region_sectors(uint32_t region_length) {
    /* A part-used sector still occupies a whole one. */
    return (region_length >> 9) + ((region_length & 0x1ff) ? 1 : 0);
}

uint32_t find_thumbnail_offset(void) {
    /* Skip the initial saved SD sector at the beginning of each slot. */
    uint32_t freeze_slot_offset = 1;

    for (unsigned char i = 0; i < freeze_region_count; i++) {
        uint32_t region_length = freeze_region_list[i].region_length & REGION_LENGTH_MASK;
        /* Two addresses because a bitstream either maps the thumbnail directly
         * at $FFD4000 or relocates it to $1000 before freezing. */
        if (freeze_region_list[i].address_base == 0x1000L ||
            freeze_region_list[i].address_base == 0xffd4000L) {
            return freeze_slot_offset;
        }
        freeze_slot_offset += region_sectors(region_length);
    }
    return FREEZE_SLOT_NOT_PRESENT;
}

uint32_t address_to_freeze_slot_offset(uint32_t address) {
    /* Skip the initial saved SD sector at the beginning of each slot. */
    uint32_t freeze_slot_offset = 1;

    for (unsigned char i = 0; i < freeze_region_count; i++) {
        uint32_t base = freeze_region_list[i].address_base;
        uint32_t relative_address = address - base;

        /* The thumbnail is examined through a fictional $FF54xxx mapping, since
         * its real base of $1000 overlaps main RAM and would match there. */
        if (base == 0x1000L && (address & 0xFFFF000L) == 0xFF54000L) {
            return (freeze_slot_offset << 9) + (address & 0xFFF);
        }

        uint32_t region_length = freeze_region_list[i].region_length & REGION_LENGTH_MASK;
        if (address < base || relative_address >= region_length) {
            freeze_slot_offset += region_sectors(region_length);
            continue;
        }

        /* In this region: step to the sector holding it, then to the byte. */
        freeze_slot_offset += relative_address >> 9;
        return (freeze_slot_offset << 9) + (relative_address & 0x1FF);
    }
    return FREEZE_SLOT_NOT_PRESENT;
}
