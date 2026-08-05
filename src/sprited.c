/*
  Based on mega65-fdisk program as a starting point.

*/

#include "fdisk_fat32.h"
#include "fdisk_hal.h"
#include "fdisk_memory.h"
#include "fdisk_screen.h"
#include "freezer.h"
#include "freezer_common.h"

#include <mega65.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Performs the $D02F knock; without it every later write to a VIC-IV
     * register such as $D054 is silently ignored and the screen mode is
     * never established. */
    mega65_fast();

    // Disable interrupts and interrupt sources
    __asm__ volatile("sei" ::: "memory");
    CIA1.icr = 0x7F;
    CIA2.icr = 0x7F;
    VICIV.imr = 0x00;
    // XXX add missing C65 AND M65 peripherals
    // C65 UART, ethernet etc

    // Bank out BASIC ROM, leave KERNAL and IO in
    POKE(0x00, 0x3F);
    POKE(0x01, 0x36);

    // No decimal mode!
    __asm__ volatile("cld");

    // Enable extended attributes so we can use reverse
    VICIV.ctrlb = VICIV.ctrlb | 0x20;

    // Correct horizontal scaling
    VICIV.chrxscl = 0x78;

    // Silence SIDs
    SID1.amp = 0;
    SID2.amp = 0;

    set_palette();

    // done in freeze_sprited.c:Initialize
    // Now find the start sector of the slot, and make a copy for safe keeping
    // slot_number = 0;
    // find_freeze_slot_start_sector(slot_number);
    // freeze_slot_start_sector = *(volatile uint32_t*)0xD681U;

    // SD or SDHC card?
    if (SD_STATUS & SD_STATUS_SDHC)
        sdhc_card = 1;
    else
        sdhc_card = 0;

    // done in freeze_sprited.c:Initialize
    // request_freeze_region_list();

    // Back to 40 column, 8-bit text mode
    VICIV.ctrlb = 0x00;
    VICIV.ctrlc = (VICIV.ctrlc & 0xa8) | 0x00;
    // Lower case
    VICIV.addr = 0x16;

    do_sprite_editor();

    // Back to 40 column mode
    VICIV.ctrlb = 0x00;
    // 256-colour char data from chip RAM, not expansion RAM
    POKE(0xD063U, 0x00);

    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
