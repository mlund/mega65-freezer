#ifndef __FDISK_MEMORY_H__
#define __FDISK_MEMORY_H__

#include <stdint.h>

void m65_io_enable(void);

/* From mega65-libc: a flat 28-bit load/store, where the DMA-based versions
 * below cost a whole DMA list per byte.  Signatures must match libc's. */
__attribute__((leaf)) uint8_t lpeek(uint32_t address);
__attribute__((leaf)) void lpoke(uint32_t address, uint8_t value);
void lcopy(long source_address, long destination_address, unsigned int count);
void lfill(long destination_address, unsigned char value, unsigned int count);

/* The SDK's PEEK/POKE are volatile, which hardware access requires. */
#include <peekpoke.h>

#endif /* __FDISK_MEMORY_H__ */