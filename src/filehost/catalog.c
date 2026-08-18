/* Reading the catalogue: its header, and one record at a time out of the
 * fixed-width run behind it. */
#include "catalog.h"

/* Assembled byte by byte rather than read through a struct: the file is
 * little-endian and unaligned, and the host build has to see the same bytes
 * the machine does. */

static constexpr uint8_t MAGIC_BYTES = 8;
static const char MAGIC[MAGIC_BYTES] = {'M', '6', '5', 'F', 'H', 'C', 'A', 'T'};
/* Both are read, and by the same code.  Version 2 filled three bytes version 1
 * had reserved and required to be zero, and zero is what both of them mean by
 * "not stated" -- so an older catalogue on a card decodes as one with no
 * category and no year rather than needing a case of its own. */
static constexpr uint8_t CATALOG_VERSION_MIN = 1;
static constexpr uint8_t CATALOG_VERSION = 2;

/* Offsets within a record.  Quoted from FILEHOST.md section 2 rather than
 * derived from the widths, so a change to one is not silently a change to the
 * other. */
static constexpr uint8_t RECORD_TITLE = 0;
static constexpr uint8_t RECORD_AUTHOR = 40;
static constexpr uint8_t RECORD_PATH = 56;
static constexpr uint8_t RECORD_KIND = 104;
static constexpr uint8_t RECORD_SIZE = 105;
static constexpr uint8_t RECORD_CATEGORY = 109;
static constexpr uint8_t RECORD_YEAR = 110;
/* The last field a record must carry, so a stride shorter than this could not
 * hold one.  Through the year even for a version 1 file: the two fields are
 * read without asking which version wrote them, so a stride that stopped short
 * of them would have this reading the next record instead. */
static constexpr uint8_t RECORD_MINIMUM = RECORD_YEAR + 2;

/* Two bytes of the file, which is little-endian throughout. */
static uint16_t le16(const uint8_t* at) {
    return (uint16_t)(at[0] | ((uint16_t)at[1] << 8));
}

/* Four of them, likewise. */
static uint32_t le32(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
        ((uint32_t)at[3] << 24);
}

/* The trailing run of `pad` is padding and anything before it is not, which
 * covers both of the format's fields: title and author are space-padded, the
 * path is NUL-padded. */
static void copy_field(char* out, const uint8_t* raw, uint8_t width, uint8_t pad) {
    while (width && raw[width - 1] == pad) {
        width--;
    }
    for (uint8_t i = 0; i < width; i++) {
        out[i] = (char)raw[i];
    }
    out[width] = 0;
}

bool catalog_header(const uint8_t* raw, struct CatalogHeader* out) {
    /* Nothing is left in `out` on refusal: a caller that carried on would
     * index with a stride and a count taken from a file this just rejected. */
    out->record_bytes = 0;
    out->record_count = 0;

    for (uint8_t i = 0; i < MAGIC_BYTES; i++) {
        if (raw[i] != (uint8_t)MAGIC[i]) {
            return false;
        }
    }
    if (raw[8] < CATALOG_VERSION_MIN || raw[8] > CATALOG_VERSION) {
        return false;
    }
    const uint16_t record_bytes = le16(&raw[9]);
    /* A stride below the fields it must carry would overlap its neighbour, and
     * zero would put every record at the same offset. */
    if (record_bytes < RECORD_MINIMUM) {
        return false;
    }
    out->record_bytes = record_bytes;
    out->record_count = le16(&raw[11]);
    return true;
}

uint32_t catalog_record_offset(uint16_t record_bytes, uint16_t index) {
    /* FILEHOST.md writes this as (i + 1) << 7, which is the same answer while
     * the record size is 128.  Taken here as a fixed 128-byte header and a
     * stride from the header, because that is what the two things the document
     * states separately -- "header - 128 bytes" and a record-size field --
     * mean apart.
     *
     * Widened before the multiply: `int` is 16 bits here, and the product
     * passes 16 bits at the 512th record. */
    return CATALOG_HEADER_BYTES + (uint32_t)index * record_bytes;
}

void catalog_record(const uint8_t* raw, struct CatalogRecord* out) {
    copy_field(out->title, &raw[RECORD_TITLE], CATALOG_TITLE_BYTES, ' ');
    copy_field(out->author, &raw[RECORD_AUTHOR], CATALOG_AUTHOR_BYTES, ' ');
    copy_field(out->path, &raw[RECORD_PATH], CATALOG_PATH_BYTES, 0);
    out->kind = raw[RECORD_KIND];
    out->size = le32(&raw[RECORD_SIZE]);
    out->category = raw[RECORD_CATEGORY];
    out->year = le16(&raw[RECORD_YEAR]);
}

const char* catalog_category_name(uint8_t category) {
    /* Indexed by the file's own value, so the order here is the format's and
     * not a choice.  A value past the end is a category invented after this was
     * written: nothing is the honest thing to draw for it. */
    static const char* const NAMES[] = {
        "",
        "GAME",
        "DEMO",
        "APPLICATION",
        "TOOL",
        "MANUAL",
        "OTHER",
        "FIRMWARE",
    };
    static_assert(sizeof NAMES / sizeof *NAMES == CatalogFirmware + 1,
        "a category the format defines with no name would index past this table");
    return category < sizeof NAMES / sizeof *NAMES ? NAMES[category] : "";
}

uint8_t catalog_category_rank(uint8_t category) {
    /* The alphabetical position of each name above, in the enumeration's own
     * order: application, demo, firmware, game, manual, other, tool.  A test
     * walks the names to check this still says so. */
    static const uint8_t RANK[] = {
        0, /* not stated, and named by nothing */
        4, /* game */
        2, /* demo */
        1, /* application */
        7, /* tool */
        5, /* manual */
        6, /* other */
        3, /* firmware */
    };
    static_assert(sizeof RANK == CatalogFirmware + 1, "a category with no place in the order");
    return category < sizeof RANK ? RANK[category] : 0;
}
