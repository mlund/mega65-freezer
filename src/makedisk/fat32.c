#include "fat32.h"

#include "dma.h"
#include "layout.h"
#include "mega65_regs.h"
#include "sdcard.h"
#include "trace.h"

#include <mega65.h>
#include <string.h>

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
    // Read out the cluster number from the FAT
    sdcard_readsector(fat1_sector + (cluster / 128));
    return *(uint32_t*)&sector_buffer[(cluster & 127) << 2];
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
            sdcard_writesector(fat1_sector + fat_sector_num, 0);
            sdcard_writesector(fat2_sector + fat_sector_num, 0);

            // chain old cluster to new cluster
            fat_sector_num = cluster / 128;
            sdcard_readsector(fat1_sector + fat_sector_num);
            *(uint32_t*)&sector_buffer[(cluster & 127) << 2] = new_cluster;
            sdcard_writesector(fat1_sector + fat_sector_num, 0);
            sdcard_writesector(fat2_sector + fat_sector_num, 0);
            return new_cluster;
        }
    }

    return 0;
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
uint32_t fat32_create_contiguous_file(char* name, uint32_t size) {
    unsigned char i;
    unsigned char sn;
    uint16_t offset;
    uint16_t j;
    uint16_t clusters;
    uint32_t k;
    uint32_t start_cluster = 0;
    uint32_t dir_cluster = 2;
    uint32_t last_dir_cluster = 2;
    uint32_t contiguous_clusters = 0;
    uint32_t fat_sector_num = 0;
    uint32_t fat_sector_count = 0;

    unsigned char have_dir_slot = 0;
    uint32_t free_dir_sector_num = 0;
    uint16_t free_dir_sector_ofs = 0;
    struct M65Tm tm = {};

    char message[FAT_NAME_TEXT];

    clusters = hw_div16_ceil(size, (uint32_t)sectors_per_cluster << 9);

    // Look for a free directory slot.
    // Also complain if the file already exists
    while (dir_cluster >= 2 && !fat_is_end_of_chain(dir_cluster)) {
        // Invariant across the sector loop, so the cluster-to-sector multiply is
        // done once per cluster on the accelerator rather than per sector in
        // software.
        const uint32_t dir_sector =
            root_dir_sector + hw_mul32(dir_cluster - 2, sectors_per_cluster);
        for (sn = 0; sn < sectors_per_cluster; sn++) {
            sdcard_readsector(dir_sector + sn);
            for (offset = 0; offset < SD_SECTOR_SIZE; offset += 32) {
                fat_name_from_entry(&sector_buffer[offset], message);
                if (!strcmp(message, name)) {
                    // ERROR: Name already exists
                    TRACE("file already exists");
                    return 0;
                }
                // Is the slot free?
                if (sector_buffer[offset] == 0) {
                    free_dir_sector_num = dir_sector + sn;
                    free_dir_sector_ofs = offset;
                    have_dir_slot = 1;
                    break;
                }
            }
            if (have_dir_slot) {
                break;
            }
        }
        // Stop once we have found a free directory slot
        if (have_dir_slot) {
            break;
        }

        // Chain to next directory cluster, and extend directory
        // if required.
        last_dir_cluster = dir_cluster;
        dir_cluster = fat32_follow_cluster(dir_cluster);
        if ((!dir_cluster) || fat_is_end_of_chain(dir_cluster)) {
            // End of directory --
            dir_cluster = fat32_allocate_cluster(last_dir_cluster);

            if ((!dir_cluster) || fat_is_end_of_chain(dir_cluster)) {
                // Disk full
                return 0;
            } else {
                // Zero out new directory cluster
                clear_sector_buffer();
                const uint32_t new_dir_sector =
                    root_dir_sector + hw_mul32(dir_cluster - 2, sectors_per_cluster);
                for (sn = 0; sn < sectors_per_cluster; sn++) {
                    sdcard_writesector(new_dir_sector + sn, 0);
                }
            }
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
        sdcard_writesector(fat1_sector + fat_sector_num + k, 0);
        sdcard_writesector(fat2_sector + fat_sector_num + k, 0);
    }

    // Build directory entry
    sdcard_readsector(free_dir_sector_num);
    // Clear entry
    for (i = 0; i < 32; i++) {
        sector_buffer[free_dir_sector_ofs + i] = 0x00;
    }
    fat_name_to_entry(name, &sector_buffer[free_dir_sector_ofs]);
    sector_buffer[free_dir_sector_ofs + 0x0b] = 0x20; // Archive bit set

    getrtc(&tm);

    // Create time 0x0e -- 0x0f, create date 0x10 -- 0x11
    *(uint16_t*)&sector_buffer[free_dir_sector_ofs + 0x0e] = fat_pack_time(&tm);
    *(uint16_t*)&sector_buffer[free_dir_sector_ofs + 0x10] = fat_pack_date(&tm);
    // Start cluster
    sector_buffer[free_dir_sector_ofs + 0x1A] = (uint8_t)start_cluster;
    sector_buffer[free_dir_sector_ofs + 0x1B] = (uint8_t)(start_cluster >> 8);
    sector_buffer[free_dir_sector_ofs + 0x14] = (uint8_t)(start_cluster >> 16);
    sector_buffer[free_dir_sector_ofs + 0x15] = start_cluster >> 24;
    // File length
    sector_buffer[free_dir_sector_ofs + 0x1C] = (size >> 0) & 0xff;
    sector_buffer[free_dir_sector_ofs + 0x1D] = (size >> 8L) & 0xff;
    sector_buffer[free_dir_sector_ofs + 0x1E] = (size >> 16L) & 0xff;
    sector_buffer[free_dir_sector_ofs + 0x1F] = (size >> 24l) & 0xff;

    sdcard_writesector(free_dir_sector_num, 0);

    return fat_cluster_first_sector(root_dir_sector, start_cluster, sectors_per_cluster);
}
