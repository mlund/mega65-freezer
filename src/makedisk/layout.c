#include "layout.h"

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
    return (uint32_t)geom->tracks * geom->sectors_per_track;
}

uint32_t disk_bam_sector(const struct DiskGeometry* geom) {
    return (uint32_t)geom->bam_track * geom->sectors_per_track;
}

uint8_t unbcd(uint8_t packed) {
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
    return data_start_sector + (cluster - 2) * sectors_per_cluster;
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
    for (uint8_t i = 0, j = 0; i < FAT_NAME_BYTES; i++, j++) {
        if (name[j] == '.') {
            i = FAT_NAME_STEM - 1;
        } else {
            entry[i] = (uint8_t)name[j];
        }
    }
}
