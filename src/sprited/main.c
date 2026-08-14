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

    /* The slot sector and the region list are set up by editor.c's Initialize,
     * not here. */
    // Back to 40 column, 8-bit text mode
    VICIV.ctrlb = 0x00;
    VICIV.ctrlc = (VICIV.ctrlc & 0xa8) | 0x00;
    // Lower case
    VICIV.addr = 0x16;

    do_sprite_editor();

    // Back to 40 column mode
    VICIV.ctrlb = 0x00;
    // 256-colour char data from chip RAM, not expansion RAM
    VICIV.scrnptr_mb = 0x00;

    mega65_dos_exechelper("FREEZER.M65");

    return 0;
}
