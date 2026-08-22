/* The disk and FAT arithmetic that touches no hardware: geometry, the clock
 * fields a directory entry stamps, and what one entry means. */

#include "layout.h"

#include "mul32.h"

#include <string.h>

/* D81 is the 1581's 80 tracks of 10 sectors a side.  D65 is the MEGA65 HD
 * format: 85 tracks of 64 sectors a side, which hyppo recognises purely by the
 * resulting file size, so the numbers here are not ours to choose. */
static const struct DiskGeometry GEOMETRY[2] = {
    [0] = {.tracks = 80, .sectors_per_track = 10 * 2, .bam_track = 39, .extension = ".D81"},
    [1] = {.tracks = 85, .sectors_per_track = 64 * 2, .bam_track = 39, .extension = ".D65"},
};

const struct DiskGeometry* disk_geometry(bool is_d65) {
    return &GEOMETRY[is_d65 ? 1 : 0];
}

uint32_t disk_total_sectors(const struct DiskGeometry* geom) {
    /* 85 x 128 at the largest, so the product is 16-bit and the multiply that
     * makes it need not be: widening first costs a 32-bit routine. */
    return (uint16_t)(geom->tracks * geom->sectors_per_track);
}

uint32_t disk_bam_sector(const struct DiskGeometry* geom) {
    return (uint16_t)(geom->bam_track * geom->sectors_per_track);
}

/* Out of line because rtc_decode() asks seven times: a byte in and a byte out
 * is one jsr, against a shift and two subtracts expanded at every one. */
__attribute__((noinline)) uint8_t unbcd(uint8_t packed) {
    const uint8_t tens = packed >> 4;
    return (uint8_t)(packed - (tens << 2) - (tens << 1));
}

void rtc_decode(const uint8_t* regs, struct M65Tm* tm) {
    tm->tm_sec = unbcd(regs[0]);
    tm->tm_min = unbcd(regs[1]);

    const uint8_t hour = regs[2];
    if (hour & RTC_HOUR_24EN) {
        tm->tm_hour = unbcd(hour & RTC_HOUR_MASK_24);
    } else {
        /* 12 AM is midnight and 12 PM is noon, so twelve wraps to zero before
         * the PM half-day is added. */
        uint8_t hour12 = unbcd(hour & RTC_HOUR_MASK_12);
        if (hour12 == 12) {
            hour12 = 0;
        }
        tm->tm_hour = hour12 + ((hour & RTC_HOUR_PM) ? 12 : 0);
    }

    /* Both the RTC and the FAT date field count the day from 1. */
    tm->tm_mday = unbcd(regs[3]);
    tm->tm_mon = unbcd(regs[4]);
    /* The RTC counts years from 2000, the tm struct from 1900. */
    tm->tm_year = unbcd(regs[5]) + 100;
}

uint16_t fat_pack_time(const struct M65Tm* tm) {
    return (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec >> 1));
}

uint16_t fat_pack_date(const struct M65Tm* tm) {
    /* DOS counts years from 1980, the tm struct from 1900. */
    return (uint16_t)(((tm->tm_year - 80) << 9) | (tm->tm_mon << 5) | tm->tm_mday);
}

bool fat_is_end_of_chain(uint32_t cluster) {
    return cluster >= FAT_END_OF_CHAIN;
}

uint16_t fat_sectors_for_clusters(uint16_t clusters) {
    uint16_t sectors = clusters / SECTORS_PER_FAT_SECTOR;
    if (clusters % SECTORS_PER_FAT_SECTOR) {
        sectors++;
    }
    return sectors;
}

uint32_t fat_cluster_first_sector(
    uint32_t data_start_sector, uint32_t cluster, uint8_t sectors_per_cluster) {
    return data_start_sector + mul32(cluster - 2, sectors_per_cluster);
}

void fat_name_from_entry(const uint8_t* entry, char* out) {
    uint8_t len = FAT_NAME_STEM;
    while (len && entry[len - 1] == ' ') {
        len--;
    }
    for (uint8_t i = 0; i < len; i++) {
        out[i] = (char)entry[i];
    }

    uint8_t ext = 3;
    while (ext && entry[FAT_NAME_STEM + ext - 1] == ' ') {
        ext--;
    }
    if (ext) {
        out[len++] = '.';
        for (uint8_t i = 0; i < ext; i++) {
            out[len + i] = (char)entry[FAT_NAME_STEM + i];
        }
        len += ext;
    }

    /* The caller's buffer is reused across entries, so this must terminate. */
    out[len] = 0;
}

void fat_name_to_entry(const char* name, uint8_t* entry) {
    memset(entry, ' ', FAT_NAME_BYTES);
    /* Stopping at the terminator, not merely at eleven bytes: a name with no
     * extension is shorter than the field, and running to the end of it read
     * past the string and left NULs where the padding should be. */
    for (uint8_t i = 0, j = 0; i < FAT_NAME_BYTES && name[j]; i++, j++) {
        if (name[j] == '.') {
            i = FAT_NAME_STEM - 1;
        } else {
            entry[i] = (uint8_t)name[j];
        }
    }
}

/* Where FAT32 put the two halves of the first cluster. */
static constexpr uint8_t FAT_ENTRY_CLUSTER_LOW = 0x1a;
static constexpr uint8_t FAT_ENTRY_CLUSTER_HIGH = 0x14;

/* Compared as text, not by packing the wanted name and matching the eleven
 * stored bytes.  The packed form is 316 bytes smaller in FILEHOST and 145 in
 * MAKEDISK and must not be taken: it fails verify_filehost_xemu.py at the exec
 * back into the freezer, reproducibly, and nothing here explains why -- every
 * entry on the test card round-trips render-then-pack except the volume label.
 * Whatever the two disagree about is something these tests do not describe. */
enum FatSlot fat_find_in_sector(const uint8_t* sector, const char* name, uint16_t* offset) {
    char text[FAT_NAME_TEXT];
    for (uint16_t at = 0; at < FAT_SECTOR_BYTES; at += FAT_ENTRY_BYTES) {
        if (!sector[at]) {
            *offset = at;
            return FatSlotFree;
        }
        /* A long name is not a name: its bytes are UTF-16 halves, so it is
         * skipped by what it says it is rather than by what it looks like. */
        if (sector[at + FAT_ENTRY_ATTRIBUTES] == FAT_ATTRIBUTE_LONG_NAME) {
            continue;
        }
        /* A deleted entry keeps all but the first byte of its name, and that
         * byte is what differs, so nothing else is needed to skip one. */
        fat_name_from_entry(&sector[at], text);
        if (!strcmp(text, name)) {
            *offset = at;
            return FatSlotFound;
        }
    }
    return FatSlotAbsent;
}

uint32_t fat_entry_first_cluster(const uint8_t* entry) {
    return (uint32_t)entry[FAT_ENTRY_CLUSTER_LOW] |
        ((uint32_t)entry[FAT_ENTRY_CLUSTER_LOW + 1] << 8) |
        ((uint32_t)entry[FAT_ENTRY_CLUSTER_HIGH] << 16) |
        ((uint32_t)entry[FAT_ENTRY_CLUSTER_HIGH + 1] << 24);
}

void fat_entry_set_first_cluster(uint8_t* entry, uint32_t cluster) {
    entry[FAT_ENTRY_CLUSTER_LOW] = (uint8_t)cluster;
    entry[FAT_ENTRY_CLUSTER_LOW + 1] = (uint8_t)(cluster >> 8);
    entry[FAT_ENTRY_CLUSTER_HIGH] = (uint8_t)(cluster >> 16);
    entry[FAT_ENTRY_CLUSTER_HIGH + 1] = (uint8_t)(cluster >> 24);
}
