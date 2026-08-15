#pragma once

/* The SD card as the tools use it: whole sectors, one at a time, through a
 * single buffer the hardware and the caller share. */

#include "dma.h"

#include <ctype.h>
#include <mega65/hal.h>
#include <stdint.h>

extern unsigned char sdhc_card;
/* The card's sector.  A FAT sector and a disk image's sector are separate
 * ideas that happen to share the word, so the prefix says which is meant. */
constexpr uint16_t SD_SECTOR_SIZE = 512;
extern uint8_t sector_buffer[SD_SECTOR_SIZE];
#define clear_sector_buffer() lfill((uint32_t)sector_buffer, 0, SD_SECTOR_SIZE)

/* Read by the card routines while they wait, set through the call below. */
extern uint8_t border_flicker;

void sdcard_visual_feedback(const uint8_t do_flicker);
void sdcard_open(void);
void sdcard_writesector(const uint32_t sector_number, uint8_t is_multi);
void sdcard_readsector(const uint32_t sector_number);
void mega65_fast(void);
void sdcard_writenextsector(void);
void sdcard_writemultidone(void);
