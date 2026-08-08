/* Host driver for src/makedisk/layout.c, used only by test/verify_layout.py.
 *
 * layout.c touches no hardware, so the host can run it and check it against
 * what mega65-core and the FAT32 format actually require -- the numbers come
 * from hyppo's dos_checkimage and the 45IO27 RTC registers, not from this code.
 *
 * Commands on stdin, one per line:
 *   geometry <0|1>                  -> tracks sectors_per_track total bam_sector ext
 *   unbcd <packed>                  -> value
 *   rtc <s> <m> <h> <d> <mo> <y>    -> year mon mday hour min sec (registers in hex)
 *   time <hour> <min> <sec>         -> packed FAT time word
 *   date <year1900> <mon> <mday>    -> packed FAT date word
 *   eoc <cluster>                   -> 1 if the chain ends here
 *   fatsectors <clusters>           -> sectors needed to hold that many entries
 *   clustersector <start> <cluster> <sectors_per_cluster> -> first sector
 *   nameout <11 chars>              -> "STEM.EXT" rendered from a directory entry
 *   namein <text>                   -> the 11 padded bytes, dots shown as _
 */

#include "../src/makedisk/layout.c"

#include <stdio.h>
#include <string.h>

int main(void) {
    char line[256];

    while (fgets(line, sizeof line, stdin)) {
        char cmd[32];
        long a = 0;
        long b = 0;
        long c = 0;

        if (sscanf(line, "%31s", cmd) != 1) {
            continue;
        }

        if (!strcmp(cmd, "geometry")) {
            sscanf(line, "%31s %ld", cmd, &a);
            const struct DiskGeometry* g = disk_geometry(a != 0);
            printf("%u %u %lu %lu %s\n",
                g->tracks,
                g->sectors_per_track,
                (unsigned long)disk_total_sectors(g),
                (unsigned long)disk_bam_sector(g),
                g->extension);
        } else if (!strcmp(cmd, "unbcd")) {
            sscanf(line, "%31s %ld", cmd, &a);
            printf("%u\n", unbcd((uint8_t)a));
        } else if (!strcmp(cmd, "rtc")) {
            /* Six raw register bytes, as read from $FFD7110 upwards. */
            unsigned r[RTC_REG_COUNT] = {0};
            sscanf(line, "%31s %x %x %x %x %x %x", cmd, &r[0], &r[1], &r[2], &r[3], &r[4], &r[5]);
            uint8_t regs[RTC_REG_COUNT];
            for (int i = 0; i < RTC_REG_COUNT; i++) {
                regs[i] = (uint8_t)r[i];
            }
            struct M65Tm tm = {};
            rtc_decode(regs, &tm);
            printf("%u %u %u %u %u %u\n",
                tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else if (!strcmp(cmd, "time")) {
            sscanf(line, "%31s %ld %ld %ld", cmd, &a, &b, &c);
            struct M65Tm tm = {.tm_hour = (uint8_t)a, .tm_min = (uint8_t)b, .tm_sec = (uint8_t)c};
            printf("%u\n", fat_pack_time(&tm));
        } else if (!strcmp(cmd, "date")) {
            sscanf(line, "%31s %ld %ld %ld", cmd, &a, &b, &c);
            struct M65Tm tm = {.tm_year = (uint16_t)a, .tm_mon = (uint8_t)b, .tm_mday = (uint8_t)c};
            printf("%u\n", fat_pack_date(&tm));
        } else if (!strcmp(cmd, "eoc")) {
            sscanf(line, "%31s %ld", cmd, &a);
            printf("%d\n", fat_is_end_of_chain((uint32_t)a) ? 1 : 0);
        } else if (!strcmp(cmd, "fatsectors")) {
            sscanf(line, "%31s %ld", cmd, &a);
            printf("%u\n", fat_sectors_for_clusters((uint16_t)a));
        } else if (!strcmp(cmd, "clustersector")) {
            sscanf(line, "%31s %ld %ld %ld", cmd, &a, &b, &c);
            printf("%lu\n",
                (unsigned long)fat_cluster_first_sector((uint32_t)a, (uint32_t)b, (uint8_t)c));
        } else if (!strcmp(cmd, "nameout")) {
            /* The 11 bytes follow the command verbatim, spaces included, so
             * they are taken by offset rather than by sscanf. */
            uint8_t entry[FAT_NAME_BYTES];
            memcpy(entry, line + strlen("nameout "), FAT_NAME_BYTES);
            /* Pre-filled so a missing terminator or a bad trim shows up: the
             * real buffer is reused across entries and is never zeroed. */
            char out[FAT_NAME_TEXT + 8];
            memset(out, 'X', sizeof out);
            out[sizeof out - 1] = '\0';
            fat_name_from_entry(entry, out);
            printf("%s\n", out);
        } else if (!strcmp(cmd, "namein")) {
            char text[32];
            sscanf(line, "%31s %31s", cmd, text);
            uint8_t entry[FAT_NAME_BYTES + 1];
            fat_name_to_entry(text, entry);
            entry[FAT_NAME_BYTES] = 0;
            for (int i = 0; i < FAT_NAME_BYTES; i++) {
                if (entry[i] == ' ') {
                    entry[i] = '_';
                }
            }
            printf("%s\n", (char*)entry);
        } else {
            printf("?\n");
        }
        fflush(stdout);
    }
    return 0;
}
