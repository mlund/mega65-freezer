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

void setup_menu_screen(void) {
    VICIV.addr = 0x15; // upper case

    // NTSC 60Hz mode for monitor compatibility?
    //  VICIV.rasline0 = 0x80;

    // Reset border widths
    POKE(0xD05CU, 80);
    POKE(0xD05DU, 0xC0);

    // No sprites
    VICIV.spr_ena = 0x00;

    // Move screen to SCREEN_ADDRESS
    VICIV.addr =
        (((CHARSET_ADDRESS - 0x8000U) >> 11) << 1) + (((SCREEN_ADDRESS - 0x8000U) >> 10) << 4);
    CIA2.pra = (CIA2.pra & 0xfc) | 0x01;

    // 16-bit text mode with full colour for chars >$FF
    // (which we will use for showing the thumbnail)
    VICIV.ctrlc = (VICIV.ctrlc & 0xa8) | 0x05;
    VICIV.linestep = 80; // $D058-$D059, bytes per row

    // Fill colour RAM with a value that won't cause problems in Super-Extended Attribute Mode
    lfill(0xff80000U, 1, 2000);
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
    POKE(0x00, 0x3F);
    POKE(0x01, 0x36);

    // No decimal mode!
    __asm__ volatile("cld");

    // Enable extended attributes so we can use reverse
    VICIV.ctrlb = VICIV.ctrlb | 0x20;

    // Correct horizontal scaling
    VICIV.chrxscl = 0x78;

    // Silence SIDs
    POKE(0xD418U, 0);
    POKE(0xD438U, 0);

    set_palette();

    // Now find the start sector of the slot, and make a copy for safe keeping
    slot_number = 0;
    freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);

    // SD or SDHC card?
    if (PEEK(0xD680U) & 0x10)
        sdhc_card = 1;
    else
        sdhc_card = 0;

    setup_menu_screen();

    request_freeze_region_list();

    do_audio_mixer();
    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
