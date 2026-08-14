/*
  Based on mega65-fdisk program as a starting point.

*/

#include "colours.h"
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

    setup_menu_screen_16bit();
    clear_colour_ram();

    request_freeze_region_list();

    // communicate changed ROM by setting specific border color
    if (do_rom_loader()) {
        VICIV.bordercol = BORDER_SIGNAL_ROM_CHANGED;
    } else {
        VICIV.bordercol = SchemeBorder;
    }

    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
