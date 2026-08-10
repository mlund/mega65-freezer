/*
  Based on mega65-fdisk program as a starting point.

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

void setup_menu_screen(void) {
    setup_menu_screen_base();

    VICIV.ctrlc = VICIV.ctrlc & VIC4_CTRLC_LEGACY_MASK;
    VICIV.linestep = SCREEN_ROW_BYTES;
    VICIV.ctrlb = VIC4_CTRLB_80_COLUMN;

    clear_colour_ram();
}

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
    CPU_PORTDDR = CPU_PORT_DDR_ALL_OUTPUTS;
    CPU_PORT = CPU_PORT_KERNAL_AND_IO;

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

    // SD or SDHC card?
    if (SD_STATUS & SD_STATUS_SDHC) {
        sdhc_card = 1;
    } else {
        sdhc_card = 0;
    }

    setup_menu_screen();

    do_megainfo();
    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
