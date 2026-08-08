#pragma once

/* The parts of MAKEDISK that touch no hardware: FAT32 record encoding and the
 * D81/D65 geometry.  Split out so the host can run them under test, the same
 * reason format.c is separate from screen.c. */

#include <stdbool.h>
#include <stdint.h>

/* 8.3 names are stored space-padded and without the dot, 8 bytes of stem
 * followed by 3 of extension. */
constexpr uint8_t FAT_NAME_BYTES = 11;
constexpr uint8_t FAT_NAME_STEM = 8;
/* "STEM.EXT" plus terminator. */
constexpr uint8_t FAT_NAME_TEXT = 13;

constexpr uint16_t SECTORS_PER_FAT_SECTOR = 128; /* 512 bytes / 4 per entry */

/* A cluster number at or above this ends a chain.  The top four bits of a FAT32
 * entry are reserved, so only the low 28 count. */
constexpr uint32_t FAT_END_OF_CHAIN = 0x0ffffff8;

/* The clock fields the directory entry stamps.  The RTC also reports weekday
 * and daylight saving; nothing here reads them. */
struct M65Tm {
    uint8_t tm_sec;   /* Seconds (0-59) */
    uint8_t tm_min;   /* Minutes (0-59) */
    uint8_t tm_hour;  /* Hours (0-23) */
    uint8_t tm_mday;  /* Day of the month (1-31), as the RTC reports it */
    uint8_t tm_mon;   /* Month (1-12), as the RTC reports it */
    uint16_t tm_year; /* Year - 1900 (in practice, never < 2000) */
};

/* One disk format.  Held in a table rather than recomputed per site: the image
 * size and the number of sectors formatted have to agree, and when they were
 * separate expressions they did not. */
struct DiskGeometry {
    uint16_t tracks;
    uint16_t sectors_per_track; /* both sides */
    uint16_t bam_track;         /* zero-based; header, BAM and directory live here */
    const char* extension;      /* including the dot */
};

const struct DiskGeometry* disk_geometry(bool is_d65);
uint32_t disk_total_sectors(const struct DiskGeometry* geom);
uint32_t disk_bam_sector(const struct DiskGeometry* geom);

/* The six RTC registers $FFD7110-$FFD7115, in that order. */
constexpr uint8_t RTC_REG_COUNT = 6;
constexpr uint8_t RTC_HOUR_24EN = 0x80; /* $FFD7112.7 RTC!HOUR24EN */
constexpr uint8_t RTC_HOUR_PM = 0x20;   /* $FFD7112.5 RTC!HOURPM */
constexpr uint8_t RTC_HOUR_MASK_24 = 0x3f;
constexpr uint8_t RTC_HOUR_MASK_12 = 0x1f;

uint8_t unbcd(uint8_t packed);
void rtc_decode(const uint8_t* regs, struct M65Tm* tm);
uint16_t fat_pack_time(const struct M65Tm* tm);
uint16_t fat_pack_date(const struct M65Tm* tm);

bool fat_is_end_of_chain(uint32_t cluster);
uint16_t fat_sectors_for_clusters(uint16_t clusters);
uint32_t fat_cluster_first_sector(
    uint32_t data_start_sector, uint32_t cluster, uint8_t sectors_per_cluster);

/* Renders a directory entry's padded name as "STEM.EXT".  out must hold
 * FAT_NAME_TEXT bytes and is always terminated. */
void fat_name_from_entry(const uint8_t* entry, char* out);
/* The reverse: "STEM.EXT" into 11 space-padded bytes. */
void fat_name_to_entry(const char* name, uint8_t* entry);
