#pragma once

/* The SD card as the tools use it: whole sectors, one at a time, through a
 * single buffer the hardware and the caller share. */

#include "dma.h"

#include <ctype.h>
#include <mega65/hal.h>
#include <stdbool.h>
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
/* Writes `sector_buffer` to `sector_number`, verified by reading it back, and
 * skipped where the card already holds those bytes.
 *
 * False means the write did not take: the card never went idle, or ten
 * attempts failed to verify.  Worth acting on -- a caller that carries on
 * writes a file that is wrong, and the wait behind this used to hang for ever
 * rather than say so.
 *
 * `is_multi` opens a CMD25 stream, and has no correct caller: this verifies by
 * reading, which a stream cannot survive.  Use sdcard_writefirstsector(). */
[[nodiscard]] bool sdcard_writesector(const uint32_t sector_number, uint8_t is_multi);
void sdcard_readsector(const uint32_t sector_number);
void mega65_fast(void);
/* A follow-on block of an open multi-block write, and the last one, which
 * closes the stream.  The buffer to write is `sector_buffer`, and the card
 * carries the address from the sdcard_writesector() that opened the stream.
 *
 * False means the card never went idle within the wait, and the stream's state
 * is then unknown: no further block may be sent into it. */
[[nodiscard]] bool sdcard_writefirstsector(const uint32_t sector_number);
[[nodiscard]] bool sdcard_writenextsector(void);
[[nodiscard]] bool sdcard_writelastsector(void);
