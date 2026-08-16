#pragma once

/* Enough FAT32 to put a disk image on the card: mount the filesystem the
 * machine boots from, and create one contiguous file in it.
 *
 * Contiguous is not tidiness: a mounted image is read by counting sectors from
 * a base (mega65-core sdcardio.vhdl:380), so hyppo refuses a fragmented one.
 * MAKEDISK creates an empty image this way and FILEHOST writes a fetched one
 * into the same kind of file, which is why this sits above both rather than
 * inside either.
 *
 * Create and remove, not shorten: a file is made at its full length before the
 * first byte of it arrives, so a write that fails part way is undone by
 * removing the file rather than by trimming it. */

#include "errors.h"

#include <stdint.h>

extern uint32_t root_dir_sector;
extern uint32_t fat1_sector;
extern uint32_t fat2_sector;

/* Contiguous so the image can be read back by sector without walking the FAT.
 * The mount state above is the filesystem this writes into, so it must follow a
 * successful fat32_open_file_system().  Returns the file's first sector, or 0
 * if the name is taken or the card has no room. */
uint32_t fat32_create_contiguous_file(const char* name, uint32_t size);

/* Where an existing file's data begins, for writing over it where it lies --
 * but only if it is exactly `size` bytes, and 0 otherwise, since a file shorter
 * than what is about to be written into it would be overrun into whatever
 * follows it on the card.
 *
 * Replacing a file this way rather than removing and remaking it is not an
 * optimisation: the writer above takes only wholly free FAT sectors, so the
 * space a delete gives back is usually not space it can take again -- and a
 * remake that then fails has destroyed the file it was replacing. */
[[nodiscard]] uint32_t fat32_file_first_sector(const char* name, uint32_t size);

/* Removes a file from the root directory and gives its clusters back to both
 * FATs.  False if the name is not there, which is not an error to a caller
 * clearing up after itself.
 *
 * The chain is followed rather than assumed to be a run: what this creates is
 * contiguous, but what another writer left is not necessarily, and freeing a
 * run that is not one would hand another file's clusters away. */
[[nodiscard]] bool fat32_delete_file(const char* name);

/* Writes `length` bytes at `offset` into a file that begins at `first_sector`.
 *
 * A sector is written whole, so `offset` must be a multiple of one and a run
 * shorter than one is padded with zeros -- which is what a file's last piece
 * is, unless its length divides exactly.  Contiguous is what makes the sector
 * plain arithmetic rather than a walk of the chain. */
void fat32_write_file_sector(
    uint32_t first_sector, uint32_t offset, const uint8_t* bytes, uint16_t length);
enum FreezerError fat32_open_file_system(void);
