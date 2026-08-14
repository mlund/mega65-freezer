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

int main(void) {
    freezer_tool_start();

    setup_menu_screen_80col();
    clear_colour_ram();

    do_megainfo();
    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
