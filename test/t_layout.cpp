/* Disk geometry, FAT records and RTC decoding.
 *
 * The numbers come from what hyppo and the 45IO27 registers require, not from
 * this code: hyppo identifies an image only by its length, so a wrong size
 * mounts and then reads back the wrong half.
 *
 * Every expectation below is a literal.  The packed FAT words in particular are
 * written out rather than shifted together the way fat_pack_time() does them --
 * an oracle that packs them the same way cannot disagree with the packer. */

#include <cstdio>
#include <cstring>
#include <string>

#include "doctest.h"

extern "C" {
#include "layout.h"
}

namespace {

/* A D81 is a 1581: 80 tracks, 10 sectors a side.  A D65 is the MEGA65 HD
 * format, 85 tracks of 64 sectors a side.  Both must match hyppo byte for
 * byte. */
constexpr uint32_t D81_SECTORS = 80 * 10 * 2;
constexpr uint32_t D65_SECTORS = 85 * 64 * 2;
static_assert(D81_SECTORS * 512 == 819200);
static_assert(D65_SECTORS * 512 == 5570560);  /* hyppo dos.asm */

/* `regs` is six raw registers as read from $FFD7110 up: seconds, minutes,
 * hours, day, month, year, all BCD. */
M65Tm decoded(const uint8_t (&regs)[RTC_REG_COUNT]) {
    struct M65Tm tm = {};
    rtc_decode(regs, &tm);
    return tm;
}

/* The real buffer is reused across directory entries and never cleared, so it
 * is poisoned here: the rendering has to terminate the string itself. */
std::string rendered(const char (&entry)[FAT_NAME_BYTES + 1]) {
    char out[FAT_NAME_TEXT + 8];
    memset(out, 'X', sizeof out);
    out[sizeof out - 1] = '\0';
    fat_name_from_entry(reinterpret_cast<const uint8_t*>(entry), out);
    return out;
}

constexpr uint16_t SECTOR_BYTES = 512;
constexpr uint8_t ENTRIES_PER_SECTOR = SECTOR_BYTES / FAT_ENTRY_BYTES;

/* A directory sector built entry by entry.  Everything past the last one placed
 * reads as unused, which is what a cleared directory cluster holds. */
struct Directory {
    uint8_t bytes[SECTOR_BYTES] = {};

    void put(uint8_t index, const char* name) {
        fat_name_to_entry(name, &bytes[index * FAT_ENTRY_BYTES]);
    }
    /* What a delete leaves behind: the name's first byte replaced. */
    void erase(uint8_t index) { bytes[index * FAT_ENTRY_BYTES] = 0xE5; }
};

struct Slot {
    FatSlot verdict;
    uint16_t offset;
};

Slot look_for(const Directory& dir, const char* name) {
    /* Poisoned, so an answer that carries no offset cannot pass by accident. */
    uint16_t offset = 0xFFFF;
    const FatSlot verdict = fat_find_in_sector(dir.bytes, name, &offset);
    return {verdict, offset};
}

/* Spaces shown as _ so padding is visible in a failure message. */
std::string packed_name(const char* text) {
    uint8_t entry[FAT_NAME_BYTES];
    fat_name_to_entry(text, entry);
    std::string out;
    for (uint8_t byte : entry) {
        out += (byte == ' ') ? '_' : static_cast<char>(byte);
    }
    return out;
}

}  // namespace

TEST_SUITE("layout") {

/* The image size and the number of sectors formatted have to agree, or the
 * back half of every image is left as whatever was on the card. */
TEST_CASE("disk geometry") {
    SUBCASE("D81 is 80 tracks x 20 sectors") {
        const struct DiskGeometry* g = disk_geometry(false);
        CHECK(g->tracks == 80);
        CHECK(g->sectors_per_track == 20);
        CHECK(disk_total_sectors(g) == D81_SECTORS);
        CHECK(disk_bam_sector(g) == 39 * 20);
        CHECK(std::string(g->extension) == ".D81");
    }
    SUBCASE("D65 is 85 tracks x 128 sectors") {
        const struct DiskGeometry* g = disk_geometry(true);
        CHECK(g->tracks == 85);
        CHECK(g->sectors_per_track == 128);
        CHECK(disk_total_sectors(g) == D65_SECTORS);
        CHECK(disk_bam_sector(g) == 39 * 128);
        CHECK(std::string(g->extension) == ".D65");
    }
}

TEST_CASE("BCD") {
    CHECK(unbcd(0x00) == 0);
    CHECK(unbcd(0x09) == 9);
    CHECK(unbcd(0x23) == 23);
    CHECK(unbcd(0x59) == 59);
    CHECK(unbcd(0x99) == 99);
}

/* $FFD7113 is the day of month and counts from 1; FAT stores the day from 1 as
 * well, so nothing may shift it in between. */
TEST_CASE("RTC decoding") {
    SUBCASE("24-hour reading, bit 7 set") {
        const uint8_t regs[RTC_REG_COUNT] = {0x47, 0x05, 0x94, 0x07, 0x03, 0x19};
        const M65Tm got = decoded(regs);
        CHECK(got.tm_year == 119);
        CHECK(got.tm_mon == 3);
        CHECK(got.tm_mday == 7);
        CHECK(got.tm_hour == 14);
        CHECK(got.tm_min == 5);
        CHECK(got.tm_sec == 47);
    }
    SUBCASE("midnight on 2000-01-01") {
        const uint8_t regs[RTC_REG_COUNT] = {0x00, 0x00, 0x80, 0x01, 0x01, 0x00};
        const M65Tm got = decoded(regs);
        CHECK(got.tm_year == 100);
        CHECK(got.tm_mon == 1);
        CHECK(got.tm_mday == 1);
        CHECK(got.tm_hour == 0);
    }
    SUBCASE("last second of 2099") {
        const uint8_t regs[RTC_REG_COUNT] = {0x59, 0x59, 0xA3, 0x31, 0x12, 0x99};
        const M65Tm got = decoded(regs);
        CHECK(got.tm_year == 199);
        CHECK(got.tm_mon == 12);
        CHECK(got.tm_mday == 31);
        CHECK(got.tm_hour == 23);
        CHECK(got.tm_min == 59);
        CHECK(got.tm_sec == 59);
    }

}

/* Bit 5 of the hour register is PM.  Noon is 12 PM and midnight is 12 AM, so a
 * bare "+12 if PM" turns 12 PM into 24 and leaves 12 AM as 12. */
TEST_CASE("RTC decoding in 12-hour mode") {
    auto hour_of = [](uint8_t packed_hour) {
        const uint8_t regs[RTC_REG_COUNT] = {0x00, 0x00, packed_hour, 0x15, 0x06, 0x20};
        return decoded(regs).tm_hour;
    };
    CHECK(hour_of(0x09) == 9);
    CHECK(hour_of(0x22) == 14);  /* 2 PM */
    CHECK(hour_of(0x32) == 12);  /* 12 PM is noon, not 24 */
    CHECK(hour_of(0x12) == 0);   /* 12 AM is midnight, not 12 */
}

/* verify_fat32.py stamps with this same moment; the odd second is deliberate,
 * since FAT stores seconds halved.  tm_year counts from 1900. */
TEST_CASE("FAT timestamps") {
    SUBCASE("time word for 14:05:47") {
        const struct M65Tm tm = {.tm_sec = 47, .tm_min = 5, .tm_hour = 14};
        CHECK(fat_pack_time(&tm) == 28855);
    }
    SUBCASE("date word for 2019-03-07") {
        const struct M65Tm tm = {.tm_mday = 7, .tm_mon = 3, .tm_year = 119};
        CHECK(fat_pack_date(&tm) == 20071);
    }
    /* Subtracting one here dated every file a day early. */
    SUBCASE("the first of the month stays 1") {
        const struct M65Tm tm = {.tm_mday = 1, .tm_mon = 1, .tm_year = 119};
        CHECK(fat_pack_date(&tm) == 20001);
    }
    SUBCASE("the last day of a year") {
        const struct M65Tm tm = {.tm_mday = 31, .tm_mon = 12, .tm_year = 100};
        CHECK(fat_pack_date(&tm) == 10655);
    }
}

/* FAT32 ends a chain at $0ffffff8 and above; anything below is a real cluster. */
TEST_CASE("end of chain") {
    CHECK_FALSE(fat_is_end_of_chain(2));           /* the root directory */
    CHECK_FALSE(fat_is_end_of_chain(0x0ffffff7));
    CHECK_FALSE(fat_is_end_of_chain(0x0f000000));
    CHECK(fat_is_end_of_chain(0x0ffffff8));
    CHECK(fat_is_end_of_chain(0x0fffffff));
}

TEST_CASE("FAT sizing") {
    CHECK(fat_sectors_for_clusters(0) == 0);
    CHECK(fat_sectors_for_clusters(1) == 1);
    CHECK(fat_sectors_for_clusters(128) == 1);
    CHECK(fat_sectors_for_clusters(129) == 2);
    CHECK(fat_sectors_for_clusters(200) == 2);
    CHECK(fat_sectors_for_clusters(256) == 2);
    CHECK(fat_sectors_for_clusters(257) == 3);
}

/* Cluster 2 is the first data cluster, and the stride is the filesystem's own
 * sectors-per-cluster -- not a hardcoded 8. */
TEST_CASE("cluster to sector") {
    CHECK(fat_cluster_first_sector(1000, 2, 8) == 1000);
    CHECK(fat_cluster_first_sector(1000, 3, 8) == 1008);
    CHECK(fat_cluster_first_sector(1000, 3, 16) == 1016);
    CHECK(fat_cluster_first_sector(1000, 4, 4) == 1008);
}

TEST_CASE("a directory entry renders as 8.3 text") {
    CHECK(rendered("TEST    D81") == "TEST.D81");    /* padded stem is trimmed */
    CHECK(rendered("AB      D65") == "AB.D65");
    CHECK(rendered("12345678D81") == "12345678.D81");  /* full width needs no trim */
    CHECK(rendered("README     ") == "README");        /* no extension, no dot */
}

/* What a directory sector means, in one place: both the writer looking for a
 * free slot and the delete looking for a name read it through this. */
TEST_CASE("finding a name in a directory sector") {
    Directory dir;
    dir.put(0, "FIRST.D81");
    dir.put(1, "SECOND.D81");
    dir.put(2, "THIRD.PRG");

    SUBCASE("a name is found where it was put") {
        CHECK(look_for(dir, "FIRST.D81").verdict == FatSlotFound);
        CHECK(look_for(dir, "FIRST.D81").offset == 0);
        CHECK(look_for(dir, "SECOND.D81").offset == 32);
        CHECK(look_for(dir, "THIRD.PRG").offset == 64);
    }
    /* Where a new entry goes, and proof the name is nowhere further on: an
     * unused entry ends the directory, so what follows is what it held before. */
    SUBCASE("a name that is not there stops at the first unused entry") {
        const Slot got = look_for(dir, "FOURTH.D81");
        CHECK(got.verdict == FatSlotFree);
        CHECK(got.offset == 96);
    }
    /* The trap: $E5 over the first byte leaves the rest of the name readable. */
    SUBCASE("a deleted entry is not a name, and does not end the scan") {
        dir.erase(1);
        CHECK(look_for(dir, "SECOND.D81").verdict == FatSlotFree);
        CHECK(look_for(dir, "THIRD.PRG").verdict == FatSlotFound);
        CHECK(look_for(dir, "THIRD.PRG").offset == 64);
    }
    SUBCASE("a sector with no unused entry says the directory goes on") {
        Directory full;
        for (uint8_t i = 0; i < ENTRIES_PER_SECTOR; i++) {
            char name[FAT_NAME_TEXT];
            snprintf(name, sizeof name, "FILE%02u.D81", i);
            full.put(i, name);
        }
        CHECK(look_for(full, "ABSENT.D81").verdict == FatSlotAbsent);
        CHECK(look_for(full, "FILE07.D81").verdict == FatSlotFound);
        CHECK(look_for(full, "FILE07.D81").offset == 7 * 32);
        CHECK(look_for(full, "FILE15.D81").offset == 15 * 32);
    }
}

/* The one 32-bit field FAT32 does not store in one piece: the high half sits at
 * $14, six words away from the low half at $1A.  A file below 65536 clusters
 * reads correctly whichever half is dropped, so the large value is the test. */
TEST_CASE("an entry's first cluster") {
    uint8_t entry[FAT_ENTRY_BYTES] = {};

    SUBCASE("a cluster above 65535 uses both halves") {
        fat_entry_set_first_cluster(entry, 0x0123ABCD);
        CHECK(entry[0x1A] == 0xCD);
        CHECK(entry[0x1B] == 0xAB);
        CHECK(entry[0x14] == 0x23);
        CHECK(entry[0x15] == 0x01);
        CHECK(fat_entry_first_cluster(entry) == 0x0123ABCD);
    }
    SUBCASE("a small cluster leaves the high half zero") {
        fat_entry_set_first_cluster(entry, 3);
        CHECK(entry[0x1A] == 3);
        CHECK(entry[0x1B] == 0);
        CHECK(entry[0x14] == 0);
        CHECK(entry[0x15] == 0);
        CHECK(fat_entry_first_cluster(entry) == 3);
    }
    /* Read back from bytes another writer left, which is what a delete meets. */
    SUBCASE("a cluster this did not write reads back") {
        entry[0x14] = 0x02;
        entry[0x15] = 0x00;
        entry[0x1A] = 0x00;
        entry[0x1B] = 0x10;
        CHECK(fat_entry_first_cluster(entry) == 0x00021000);
    }
}

TEST_CASE("8.3 text packs back into an entry") {
    CHECK(packed_name("TEST.D81") == "TEST____D81");   /* the dot becomes padding */
    CHECK(packed_name("AB.D65") == "AB______D65");
    CHECK(packed_name("12345678.D81") == "12345678D81");
    /* Shorter than the field: what follows the name is padding, and reading
     * past the terminator to fill it put NULs there instead. */
    CHECK(packed_name("README") == "README_____");
    CHECK(packed_name("A") == "A__________");
}

}  // TEST_SUITE
