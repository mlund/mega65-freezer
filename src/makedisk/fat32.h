#pragma once

/* Enough FAT32 to put a disk image on the card: mount the filesystem the
 * machine boots from, and create one contiguous file in it. */

#include "errors.h"

#include <stdint.h>

extern uint32_t root_dir_sector;
extern uint32_t fat1_sector;
extern uint32_t fat2_sector;

/* Contiguous so the image can be read back by sector without walking the FAT.
 * The mount state above is the filesystem this writes into, so it must follow a
 * successful fat32_open_file_system().  Returns the file's first sector, or 0
 * if the name is taken or the card has no room. */
uint32_t fat32_create_contiguous_file(char* name, uint32_t size);
enum FreezerError fat32_open_file_system(void);
