/* Host driver for src/slotmap.c, used only by test/verify_slotmap.py.
 *
 * slotmap.c touches no hardware -- it only walks the region table hyppo leaves
 * behind -- so the host can run it and check it against what freeze.asm's
 * freeze_mem_list actually declares, rather than against this code's own
 * arithmetic.
 *
 * Commands on stdin, one per line:
 *   count <n>                  -> set how many regions are live
 *   region <i> <base> <length> -> define one region (hex, no $ or 0x)
 *   offset <address>           -> byte offset in the slot, or NONE
 *   sector <address>           -> the sector that offset falls in, or NONE
 *   thumb                      -> thumbnail sector index, or NONE
 */

#include "../src/slotmap.c"

#include <stdio.h>
#include <string.h>

int main(void) {
    char line[128];

    while (fgets(line, sizeof line, stdin)) {
        unsigned index;
        unsigned long base;
        unsigned long length;
        unsigned long address;

        if (sscanf(line, "count %u", &index) == 1) {
            freeze_region_count = (unsigned char)index;
        } else if (sscanf(line, "region %u %lx %lx", &index, &base, &length) == 3) {
            freeze_region_list[index].address_base = (uint32_t)base;
            freeze_region_list[index].region_length = (uint32_t)length;
        } else if (sscanf(line, "offset %lx", &address) == 1) {
            uint32_t at = address_to_freeze_slot_offset((uint32_t)address);
            if (at == FREEZE_SLOT_NOT_PRESENT) {
                puts("NONE");
            } else {
                printf("%lx\n", (unsigned long)at);
            }
        } else if (sscanf(line, "sector %lx", &address) == 1) {
            uint32_t at = address_to_freeze_slot_offset((uint32_t)address);
            if (at == FREEZE_SLOT_NOT_PRESENT) {
                puts("NONE");
            } else {
                printf("%lx\n", (unsigned long)(at >> 9));
            }
        } else if (strncmp(line, "thumb", 5) == 0) {
            uint32_t at = find_thumbnail_offset();
            if (at == FREEZE_SLOT_NOT_PRESENT) {
                puts("NONE");
            } else {
                printf("%lx\n", (unsigned long)at);
            }
        } else {
            puts("?");
        }
    }
    return 0;
}
