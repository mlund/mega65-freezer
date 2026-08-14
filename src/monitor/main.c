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
    freezer_tool_start();

    // Now find the start sector of the slot, and make a copy for safe keeping
    slot_number = 0;
    freeze_slot_start_sector = read_freeze_slot_start_sector(slot_number);

    /* Only the border width and the sprites: console.c's setup_screen(),
     * called moments later, sets the text mode and clears both planes. */
    setup_menu_screen_base();

    request_freeze_region_list();

    freeze_monitor();
    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
