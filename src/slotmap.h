#pragma once

/* Where a frozen address lives inside the freeze slot.
 *
 * The slot is a run of SD sectors, not memory, so a 28-bit frozen address is a
 * key into the region table the hypervisor left behind rather than a location.
 * The regions sit back to back in the slot, each rounded up to a whole sector,
 * so finding an address means walking them and accumulating.
 *
 * Split from slot.c because none of this touches hardware and the host can run
 * it under test -- the same reason makedisk/layout.c is separate from the rest
 * of MAKEDISK. */

#include <stdint.h>

/* Only the lower 24 bits of region_length are valid; the space a region takes
 * in the slot is that length rounded up to a whole 512-byte sector. */
constexpr uint32_t REGION_LENGTH_MASK = 0x7FFFFF;

struct FreezeRegion {
    uint32_t address_base;
    union {
        uint32_t region_length;
        struct {
            unsigned char skip[3];
            unsigned char freeze_prep;
        };
    };
};

constexpr uint8_t MAX_REGIONS = 256 / sizeof(struct FreezeRegion);

/* Filled by request_freeze_region_list() from the hypervisor, and read here. */
extern struct FreezeRegion freeze_region_list[MAX_REGIONS];
extern unsigned char freeze_region_count;

/* Returned when no region holds the address.  A byte offset can never be this,
 * so callers test against it rather than carrying a separate flag. */
constexpr uint32_t FREEZE_SLOT_NOT_PRESENT = 0xFFFFFFFFUL;

/* Byte offset of `address` within the slot, counting from the slot's start and
 * past the saved SD sector, or FREEZE_SLOT_NOT_PRESENT. */
uint32_t address_to_freeze_slot_offset(uint32_t address);

/* Sector index of the thumbnail region within the slot, or
 * FREEZE_SLOT_NOT_PRESENT.  A sector rather than a byte offset: the caller
 * reads whole sectors from it. */
uint32_t find_thumbnail_offset(void);
