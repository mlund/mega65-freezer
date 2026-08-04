#ifndef __FDISK_MEMORY_H__
#define __FDISK_MEMORY_H__

void m65_io_enable(void);
unsigned char lpeek(long address);
void lpoke(long address, unsigned char value);
void lcopy(long source_address, long destination_address, unsigned int count);
void lcopy_safe(unsigned long src, unsigned long dst, unsigned int count);
void lfill(long destination_address, unsigned char value, unsigned int count);

#include "vic4.h"
/* The SDK's PEEK/POKE are volatile, which hardware access requires. */
#include <peekpoke.h>

#endif /* __FDISK_MEMORY_H__ */