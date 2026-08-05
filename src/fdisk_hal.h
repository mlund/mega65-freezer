#ifndef __FDISK_HAL_H__
#define __FDISK_HAL_H__

#include "fdisk_memory.h"

#include <ctype.h>
#include <mega65/hal.h>
#include <stdint.h>

extern unsigned char sdhc_card;
#define SECTOR_SIZE 512
extern uint8_t sector_buffer[SECTOR_SIZE];
#define clear_sector_buffer() lfill((uint32_t)sector_buffer, 0, SECTOR_SIZE)

extern uint8_t hal_border_flicker;

void sdcard_visual_feedback(const uint8_t do_flicker);
void sdcard_open(void);
void sdcard_writesector(const uint32_t sector_number, uint8_t is_multi);
void sdcard_readsector(const uint32_t sector_number);
void mega65_fast(void);
void sdcard_writenextsector(void);
void sdcard_writemultidone(void);

#endif /* __FDISK_HAL_H__ */
