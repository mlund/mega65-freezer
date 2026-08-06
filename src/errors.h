#pragma once

#include <stdint.h>

/* What a call into the freeze slot or the filesystem reports back.
 *
 * The values are the ones the code already returned, so naming them changes no
 * behaviour.  Zero is success, which is why callers should say `!= FreezerOk`:
 * the older spelling `if (!freeze_fetch_sector(...))` reads as "if the fetch
 * failed" and means the opposite.
 *
 * ATTACH_ERROR ($EF) is not here.  helper.S returns it, and helper.S is
 * uppercase-.S so it #includes helper.h and the assembler needs a real macro. */
enum FreezerError : uint8_t {
    FreezerOk = 0,
    /* The address is not inside the freeze slot -- unfrozen memory, or a
     * region the freezer did not capture. */
    FreezerNotFrozen = 0x55,
    /* More than a sector's worth was asked for in one go; the partial
     * transfers work within a single sector. */
    FreezerCountTooLarge = 0x56,
    /* The card's first sector has no $55AA signature, so there is no partition
     * table to read. */
    FreezerBadFilesystem = 255,
};
