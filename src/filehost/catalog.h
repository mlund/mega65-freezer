#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The FileHost catalogue: one fixed-width file, so a client indexes with
 * arithmetic and parses nothing.  The format is ether65's docs/FILEHOST.md
 * section 2, and this file is the only thing here that knows it.  No hardware,
 * so it compiles for the host and is tested there.
 *
 * Text in the file is printable ASCII rather than PETSCII, deliberately: it
 * keeps the file renderable by a cron job that need not know what a MEGA65 is.
 * Converting it is screencode.h's job. */

/* Field widths the format fixes.  The distance between records is not among
 * them -- it comes from the header, so a later version with wider records
 * still indexes correctly. */
constexpr uint8_t CATALOG_TITLE_BYTES = 40;
constexpr uint8_t CATALOG_AUTHOR_BYTES = 16;
constexpr uint8_t CATALOG_PATH_BYTES = 48;
constexpr uint8_t CATALOG_HEADER_BYTES = 128;

enum CatalogKind : uint8_t {
    CatalogPrg = 0,
    CatalogD81 = 1,
};

/* What a title is, as version 2 enumerates it.  Not text in the file: the
 * column is eleven characters wide and upstream's spelling is not this client's
 * business.  Zero is "not stated", which is also what version 1 left in the
 * byte, so one decoder reads both versions. */
enum CatalogCategory : uint8_t {
    CatalogNoCategory = 0,
    CatalogGame = 1,
    CatalogDemo = 2,
    CatalogApplication = 3,
    CatalogTool = 4,
    CatalogManual = 5,
    CatalogOther = 6,
    CatalogFirmware = 7,
};

struct CatalogHeader {
    uint16_t record_bytes;
    uint16_t record_count;
};

/* One record, unpacked: the padding gone and every string terminated. */
struct CatalogRecord {
    char title[CATALOG_TITLE_BYTES + 1];
    char author[CATALOG_AUTHOR_BYTES + 1];
    char path[CATALOG_PATH_BYTES + 1];
    uint8_t kind;
    uint32_t size;
    /* Both 0 where the file did not say, which is every record of a version 1
     * catalogue and any record a renderer had nothing to put here. */
    uint8_t category;
    uint16_t year;
};

/* What to draw for a category, and "" for one this client has no name for --
 * including a value a later renderer invents, which is shown as nothing rather
 * than as the wrong thing. */
[[nodiscard]] const char* catalog_category_name(uint8_t category);

/* True, and fills `out`, when the 128 bytes are a catalogue this client can
 * read.  Refuses an unknown version, and a record size it could not index. */
[[nodiscard]] bool catalog_header(const uint8_t* raw, struct CatalogHeader* out);

/* Where record `index` starts.  32-bit because 512 records of 128 bytes is
 * already past what 16 bits hold. */
[[nodiscard]] uint32_t catalog_record_offset(uint16_t record_bytes, uint16_t index);

/* Unpacks one record's fields into `out`. */
void catalog_record(const uint8_t* raw, struct CatalogRecord* out);
