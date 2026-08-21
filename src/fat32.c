/* Finding the filesystem the machine boots from, and laying a file into it
 * whose sectors run back to back. */

#include "fat32.h"

#include "dma.h"
#include "layout.h"
#include "mega65_regs.h"
#include "sdcard.h"
#include "trace.h"

#include <mega65.h>
#include <string.h>

/* layout.c reads directory and FAT sectors out of the same buffer this fills,
 * and cannot see sdcard.h to learn how big one is. */
static_assert(FAT_SECTOR_BYTES == SD_SECTOR_SIZE, "layout.c would scan the wrong length");

uint32_t root_dir_sector = 0;
uint32_t fat1_sector = 0;
uint32_t fat2_sector = 0;
static uint8_t sectors_per_cluster = 0;

static void parse_partition_entry(uint8_t i) {
    const uint16_t offset = 0x1be + (i << 4);

    // Only the partition type and the LBA start are used.  The CHS geometry at
    // offsets 1-3 and 5-7 is what a BIOS needed; nothing here reads it, and the
    // partition size is implied by the FAT.
    const uint8_t partition_type = sector_buffer[offset + 4];
    if (partition_type != 0x0b && partition_type != 0x0c) {
        return; // not FAT32
    }

    // The card is little-endian, as is the CPU, so the multi-byte fields are
    // read straight out of the buffer.
    const uint32_t lba_start = *(uint32_t*)&sector_buffer[offset + 8];

    sdcard_readsector(lba_start);
    const uint16_t reserved_sectors = sector_buffer[0x0e] + (sector_buffer[0x0f] << 8);
    sectors_per_cluster = sector_buffer[0x0d];
    const uint8_t fat_copies = sector_buffer[0x10];
    const uint32_t sectors_per_fat = *(uint32_t*)&sector_buffer[0x24];

    // FATs begin at partition + reserved sectors; the root directory is
    // cluster 2, which begins after the last FAT copy.
    fat1_sector = lba_start + reserved_sectors;
    fat2_sector = fat1_sector + sectors_per_fat;
    root_dir_sector = fat1_sector + hw_mul32(sectors_per_fat, fat_copies);
}

/* The board the core is running on, as $D3629 reports it. */
enum Target : uint8_t {
    TargetUnknown = 0,
    TargetMega65R1 = 1,
    TargetMega65R2 = 2,
    TargetMega65R3 = 3,
    TargetMegaphoneR1 = 0x21,
    TargetNexys4 = 0x40,
    TargetNexys4Ddr = 0x41,
    TargetNexys4DdrWidget = 0x42,
    TargetWukong = 0xFD,
    TargetSimulation = 0xFE,
};

static enum Target detect_target(void) {
    return (enum Target)lpeek(M65_MODEL_ID);
}

/* The RTC updates asynchronously, so a single read can catch a field mid-carry;
 * three consecutive agreeing reads mean the counter was not moving. */
static uint8_t lpeek_debounced(uint32_t address) {
    uint8_t first;
    uint8_t second;
    uint8_t third;
    do {
        first = lpeek(address);
        second = lpeek(address);
        third = lpeek(address);
    } while (first != second || first != third);
    return first;
}

/* Leaves *tm alone unless it can read the clock, so the caller's initialiser is
 * what a board without a known RTC ends up stamping.  The decoding is in
 * layout.c, where the host tests can reach it. */
static void getrtc(struct M65Tm* tm) {
    const enum Target target = detect_target();
    if (target != TargetMega65R2 && target != TargetMega65R3) {
        return; // no RTC this code knows how to read
    }

    uint8_t regs[RTC_REG_COUNT];
    for (uint8_t i = 0; i < RTC_REG_COUNT; i++) {
        regs[i] = lpeek_debounced(RTC_SECONDS + i);
    }
    rtc_decode(regs, tm);
}

enum FreezerError fat32_open_file_system(void) {
    sdcard_readsector(0);
    if ((sector_buffer[0x1fe] != 0x55) || (sector_buffer[0x1ff] != 0xAA)) {
        TRACE("no $55AA signature on sector 0");
        return FreezerBadFilesystem;
    }
    for (uint8_t i = 0; i < 4; i++) {
        parse_partition_entry(i);
    }
    return FreezerOk;
}

static uint32_t fat32_follow_cluster(uint32_t cluster) {
    // Read out the cluster number from the FAT, dropping the reserved top four
    // bits: a formatter may leave them set, and a link read with them compares
    // as neither a cluster number nor an end-of-chain marker -- which would
    // stop a directory walk early and report a file that exists as absent.
    sdcard_readsector(fat1_sector + (cluster / SECTORS_PER_FAT_SECTOR));
    return *(uint32_t*)&sector_buffer[(cluster & (SECTORS_PER_FAT_SECTOR - 1)) << 2] &
        FAT_CLUSTER_MASK;
}

static uint32_t fat32_allocate_cluster(uint32_t cluster) {
    uint32_t new_cluster;
    uint32_t fat_sector_num;
    uint16_t i;

    // Find free cluster
    for (fat_sector_num = 0; fat_sector_num < (fat2_sector - fat1_sector); fat_sector_num++) {
        sdcard_readsector(fat1_sector + fat_sector_num);
        for (i = 0; i < SD_SECTOR_SIZE; i += 4) {
            if (*(uint32_t*)&sector_buffer[i] == 0) {
                break;
            }
        }
        if (i < SD_SECTOR_SIZE) {
            // Found new free cluster, so place end-of-chain marker on it
            new_cluster = fat_sector_num * 128 + (i >> 2);
            *(uint32_t*)&sector_buffer[i] = 0x0fffffff;
            (void)sdcard_writesector(fat1_sector + fat_sector_num, 0);
            (void)sdcard_writesector(fat2_sector + fat_sector_num, 0);

            // chain old cluster to new cluster
            fat_sector_num = cluster / 128;
            sdcard_readsector(fat1_sector + fat_sector_num);
            *(uint32_t*)&sector_buffer[(cluster & 127) << 2] = new_cluster;
            (void)sdcard_writesector(fat1_sector + fat_sector_num, 0);
            (void)sdcard_writesector(fat2_sector + fat_sector_num, 0);
            return new_cluster;
        }
    }

    return 0;
}

/* Which FAT sector the buffer holds, and whether it holds one at all -- a flag
 * beside it rather than a biased number, since sector 0 is a real answer. */
static uint32_t fat_held_at;
static bool fat_held;

/* Both FATs, since either copy is the one a reader may believe. */
static void flush_fat(void) {
    if (fat_held) {
        (void)sdcard_writesector(fat1_sector + fat_held_at, 0);
        (void)sdcard_writesector(fat2_sector + fat_held_at, 0);
    }
}

/* Gives a chain's clusters back, a FAT sector at a time rather than a cluster at
 * a time: a contiguous 800KB file is two sectors, not two hundred reads.
 *
 * The count is what stops a damaged chain that leads back into itself, and is
 * flat rather than the FAT's own cluster count: 65535 clusters is a quarter of
 * a gigabyte at the usual 4KB, far past anything this deletes, and a 16-bit
 * counter measured 132 bytes less than the 32-bit one it takes to be exact. */
static void free_chain(uint32_t cluster) {
    uint16_t left = 0xffff;
    fat_held = false;
    while (left-- && cluster >= 2 && !fat_is_end_of_chain(cluster)) {
        const uint32_t sector = cluster / SECTORS_PER_FAT_SECTOR;
        if (!fat_held || fat_held_at != sector) {
            flush_fat();
            sdcard_readsector(fat1_sector + sector);
            fat_held_at = sector;
            fat_held = true;
        }
        uint32_t* const link =
            (uint32_t*)&sector_buffer[(cluster & (SECTORS_PER_FAT_SECTOR - 1)) << 2];
        cluster = *link & FAT_CLUSTER_MASK;
        *link = 0;
    }
    flush_fat();
}

/* What find_in_root() answers with, beside its verdict.  Held here rather than
 * returned through pointers: four out-parameters measured 51 bytes more than
 * these do, the 6502 addressing a global directly and a pointer not. */
static uint32_t found_sector;
static uint16_t found_offset;
static uint32_t last_dir_cluster;

/* The root directory, sector by sector, for a name.
 *
 * FatSlotFound and FatSlotFree answer where -- an entry, or where a new one
 * goes -- in found_sector and found_offset, and leave that sector in the
 * buffer, which is what both callers go on to write to.  FatSlotAbsent means
 * the directory ran out with neither, so last_dir_cluster is the one to chain
 * another onto; only creating cares.
 *
 * One walk rather than two: removing a file asks the same question as creating
 * one, and a second copy is a second place for the cluster arithmetic to be
 * wrong.
 *
 * Out of line: MAKEDISK has only the one caller, and letting it inline there
 * measured 104 bytes more. */
__attribute__((noinline)) static enum FatSlot find_in_root(const char* name) {
    uint32_t cluster = 2;
    while (cluster >= 2 && !fat_is_end_of_chain(cluster)) {
        // Invariant across the sector loop, so the cluster-to-sector multiply is
        // done once per cluster on the accelerator rather than per sector in
        // software.
        const uint32_t base = root_dir_sector + hw_mul32(cluster - 2, sectors_per_cluster);
        for (uint8_t sn = 0; sn < sectors_per_cluster; sn++) {
            sdcard_readsector(base + sn);
            const enum FatSlot slot = fat_find_in_sector(sector_buffer, name, &found_offset);
            if (slot != FatSlotAbsent) {
                found_sector = base + sn;
                return slot;
            }
        }
        last_dir_cluster = cluster;
        cluster = fat32_follow_cluster(cluster);
    }
    return FatSlotAbsent;
}

/* The length FAT32 records for a file, at offset $1C of its entry. */
static constexpr uint8_t FAT_ENTRY_SIZE_AT = 0x1c;

/* Whether `size` bytes from `first` really do run back to back.
 *
 * Asked rather than assumed: a caller that writes over a file writes forward
 * from one sector, so a file another writer left in two pieces would have the
 * second half of the write land on whatever follows the first piece.  A file
 * this module made is contiguous, and hyppo will not mount one that is not --
 * but neither of those is a fact this function can check, and the cost of
 * being wrong is somebody else's file. */
static bool contiguous_run(uint32_t first, uint32_t size) {
    uint16_t clusters = hw_div16_ceil(size, (uint32_t)sectors_per_cluster << 9);
    if (first < 2 || !clusters) {
        return false;
    }
    uint32_t cluster = first;
    while (--clusters) {
        if (fat32_follow_cluster(cluster) != cluster + 1) {
            return false;
        }
        cluster++;
    }
    /* And it ends where the length says, rather than running on into a longer
     * file that merely starts the same way. */
    return fat_is_end_of_chain(fat32_follow_cluster(cluster));
}

uint32_t fat32_file_first_sector(const char* name, uint32_t size) {
    if (find_in_root(name) != FatSlotFound) {
        return 0;
    }
    if (*(uint32_t*)&sector_buffer[found_offset + FAT_ENTRY_SIZE_AT] != size) {
        return 0;
    }
    /* Read before the walk below reads FAT sectors over the directory one. */
    const uint32_t first = fat_entry_first_cluster(&sector_buffer[found_offset]);
    if (!contiguous_run(first, size)) {
        return 0;
    }
    return fat_cluster_first_sector(root_dir_sector, first, sectors_per_cluster);
}

bool fat32_delete_file(const char* name) {
    if (find_in_root(name) != FatSlotFound) {
        return false;
    }

    // Read before it is stamped over: the entry is the only record of where the
    // chain begins, and the search left its sector in the buffer.
    const uint32_t first = fat_entry_first_cluster(&sector_buffer[found_offset]);
    sector_buffer[found_offset] = FAT_ENTRY_DELETED;
    (void)sdcard_writesector(found_sector, 0);

    // The entry goes first: clusters freed under a name still in the directory
    // would be handed to the next file while this one can still be opened.
    free_chain(first);
    return true;
}

/*
  Create a file in the root directory of the new FAT32 filesystem
  with the indicated name and size.

  The file will be created contiguous on disk, and the first
  sector of the created file returned.

  The root directory is the start of cluster 2, and clusters are
  assumed to be 4KB in size, to keep things simple.

  XXX -- Should allow creation of files in sub-directories

*/
bool fat32_write_file_sector(
    uint32_t first_sector, uint32_t offset, const uint8_t* bytes, uint16_t length) {
    lcopy_near(bytes, sector_buffer, length);
    if (length < SD_SECTOR_SIZE) {
        lfill_near(&sector_buffer[length], 0, SD_SECTOR_SIZE - length);
    }
    return sdcard_writesector(first_sector + offset / SD_SECTOR_SIZE, 0);
}

/* A CMD25 cannot be checked between sectors: any read abandons its stream.
 * Keep the source in high RAM, close the stream, then verify every sector. */
bool fat32_write_file_sectors(
    const uint32_t first_sector, const uint32_t offset, const uint8_t* bytes, const uint8_t count) {
    const uint32_t first = first_sector + offset / SD_SECTOR_SIZE;
    bool stream_open = false;

    for (uint8_t block = 0; block < count; block++) {
        const uint16_t byte_offset = (uint16_t)block * SD_SECTOR_SIZE;
        lcopy_near(&bytes[byte_offset], sector_buffer, SD_SECTOR_SIZE);
        const bool wrote = block == 0 ? sdcard_writefirstsector(first)
            : block + 1 == count ? sdcard_writelastsector()
                                 : sdcard_writenextsector();
        if (!wrote) {
            if (stream_open) {
                sdcard_writeabort();
            }
            return false;
        }
        stream_open = true;
    }

    for (uint8_t block = 0; block < count; block++) {
        const uint16_t byte_offset = (uint16_t)block * SD_SECTOR_SIZE;
        sdcard_readsector(first + block);
        for (uint16_t byte = 0; byte < SD_SECTOR_SIZE; byte++) {
            if (sector_buffer[byte] != bytes[byte_offset + byte]) {
                return false;
            }
        }
    }
    return true;
}

uint32_t fat32_create_contiguous_file(const char* name, uint32_t size) {
    uint16_t offset;
    uint16_t j;
    uint16_t clusters;
    uint32_t k;
    uint32_t start_cluster = 0;
    uint32_t contiguous_clusters = 0;
    uint32_t fat_sector_num = 0;
    uint32_t fat_sector_count = 0;

    struct M65Tm tm = {};

    clusters = hw_div16_ceil(size, (uint32_t)sectors_per_cluster << 9);

    // Where the entry goes, and a refusal if the name is taken.
    const enum FatSlot slot = find_in_root(name);
    if (slot == FatSlotFound) {
        TRACE("file already exists");
        return 0;
    }
    if (slot == FatSlotAbsent) {
        // Every slot taken, so the directory gets another cluster.  Zeroed, so
        // its first entry is the free one -- no need to go looking again.
        /* Zero is the only failure it can answer: what it returns otherwise is
         * an index into the FAT it just searched, which cannot reach the
         * end-of-chain values. */
        const uint32_t dir_cluster = fat32_allocate_cluster(last_dir_cluster);
        if (!dir_cluster) {
            TRACE("no room to extend the directory");
            return 0;
        }
        clear_sector_buffer();
        found_sector = root_dir_sector + hw_mul32(dir_cluster - 2, sectors_per_cluster);
        found_offset = 0;
        for (uint8_t sn = 0; sn < sectors_per_cluster; sn++) {
            (void)sdcard_writesector(found_sector + sn, 0);
        }
    }

    // Find where we have enough contiguous space
    contiguous_clusters = 0;
    start_cluster = 0;
    for (fat_sector_num = 0; fat_sector_num <= (uint32_t)(fat2_sector - fat1_sector);
        fat_sector_num++) {

        // This can take a while if the disk is full, because we use a naive search.
        // So show the user that something is happening.
        // XXX Use FAT32's hint of first cluster free _and_ then update it!
        VICIV.bordercol = VICIV.screencol;

        sdcard_readsector(fat1_sector + fat_sector_num);

        // Skip any FAT sectors with allocated clusters
        for (j = 0; j < SD_SECTOR_SIZE; j++) {
            if (sector_buffer[j]) {
                break;
            }
        }
        if (j != SD_SECTOR_SIZE) {
            // Reset count of contiguous clusters
            contiguous_clusters = 0;
            continue;
        } else {
            // Start from here
            if (!contiguous_clusters) {
                start_cluster = fat_sector_num * 128;
            }
            contiguous_clusters += 128;
        }
        if (contiguous_clusters >= clusters) {
            break;
        }
    }

    // Abort if the disk is full
    if (contiguous_clusters < clusters) {
        TRACE("no free contiguous space");
        return 0;
    }

    // Write cluster chain into both FATs
    fat_sector_num = start_cluster / SECTORS_PER_FAT_SECTOR;
    fat_sector_count = fat_sectors_for_clusters(clusters);
    for (k = 0; k < fat_sector_count; k++) {
        // Entries past the end of the chain must read as free, and the buffer
        // still holds the previous sector.
        clear_sector_buffer();
        for (offset = 0; offset < SD_SECTOR_SIZE; offset += 4) {
            const uint16_t entry = (uint16_t)((k << 7) + (offset >> 2));
            if (entry < clusters) {
                *(uint32_t*)&sector_buffer[offset] =
                    (entry == clusters - 1) ? FAT_END_OF_CHAIN : start_cluster + entry + 1;
            }
        }
        // Write FAT sector to both FATs
        (void)sdcard_writesector(fat1_sector + fat_sector_num + k, 0);
        (void)sdcard_writesector(fat2_sector + fat_sector_num + k, 0);
    }

    // Build directory entry
    sdcard_readsector(found_sector);
    memset(&sector_buffer[found_offset], 0, FAT_ENTRY_BYTES);
    fat_name_to_entry(name, &sector_buffer[found_offset]);
    sector_buffer[found_offset + 0x0b] = 0x20; // Archive bit set

    getrtc(&tm);

    // Create time 0x0e -- 0x0f, create date 0x10 -- 0x11
    *(uint16_t*)&sector_buffer[found_offset + 0x0e] = fat_pack_time(&tm);
    *(uint16_t*)&sector_buffer[found_offset + 0x10] = fat_pack_date(&tm);
    fat_entry_set_first_cluster(&sector_buffer[found_offset], start_cluster);
    // File length
    sector_buffer[found_offset + 0x1C] = (size >> 0) & 0xff;
    sector_buffer[found_offset + 0x1D] = (size >> 8L) & 0xff;
    sector_buffer[found_offset + 0x1E] = (size >> 16L) & 0xff;
    sector_buffer[found_offset + 0x1F] = (size >> 24l) & 0xff;

    (void)sdcard_writesector(found_sector, 0);

    return fat_cluster_first_sector(root_dir_sector, start_cluster, sectors_per_cluster);
}
