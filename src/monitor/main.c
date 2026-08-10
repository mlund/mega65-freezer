/*
  Based on mega65-fdisk program as a starting point.

  MEGA65 only: hardware registers and the freeze slot.
*/

#include "common.h"
#include "dma.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

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
    CIA1.icr = CIA_ICR_DISABLE_ALL;
    CIA2.icr = CIA_ICR_DISABLE_ALL;
    VICIV.imr = 0x00;
    // XXX add missing C65 AND M65 peripherals
    // C65 UART, ethernet etc

    CPU_PORTDDR = CPU_PORT_DDR_ALL_OUTPUTS;
    CPU_PORT = CPU_PORT_KERNAL_AND_IO;

    // No decimal mode!
    __asm__ volatile("cld");

    VICIV.ctrlb = VICIV.ctrlb | VIC4_CTRLB_EXTENDED_ATTRIBUTES;
    VICIV.chrxscl = VIC4_CHRXSCL_80_COLUMN;

    // Silence SIDs
    SID1.amp = 0;
    SID2.amp = 0;

    set_palette();

    // Now find the start sector of the slot, and make a copy for safe keeping
    slot_number = 0;
    freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);

    // SD or SDHC card?
    if (SD_STATUS & SD_STATUS_SDHC) {
        sdhc_card = 1;
    } else {
        sdhc_card = 0;
    }

    /* Only the border width and the sprites: console.c's setup_screen(),
     * called moments later, sets the text mode and clears both planes. */
    setup_menu_screen_base();

    request_freeze_region_list();

    freeze_monitor();
    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
