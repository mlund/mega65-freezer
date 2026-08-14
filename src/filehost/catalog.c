#include "catalog.h"

/* Assembled byte by byte rather than read through a struct: the file is
 * little-endian and unaligned, and the host build has to see the same bytes
 * the machine does. */

static constexpr uint8_t MAGIC_BYTES = 8;
static const char MAGIC[MAGIC_BYTES] = {'M', '6', '5', 'F', 'H', 'C', 'A', 'T'};
static constexpr uint8_t CATALOG_VERSION = 1;

/* Offsets within a record.  Quoted from FILEHOST.md section 2 rather than
 * derived from the widths, so a change to one is not silently a change to the
 * other. */
static constexpr uint8_t RECORD_TITLE = 0;
static constexpr uint8_t RECORD_AUTHOR = 40;
static constexpr uint8_t RECORD_PATH = 56;
static constexpr uint8_t RECORD_KIND = 104;
static constexpr uint8_t RECORD_SIZE = 105;
/* The last field a record must carry, so a stride shorter than this could not
 * hold one. */
static constexpr uint8_t RECORD_MINIMUM = RECORD_SIZE + 4;

static uint16_t le16(const uint8_t* at) {
    return (uint16_t)(at[0] | ((uint16_t)at[1] << 8));
}

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
    for (uint8_t i = 0; i < MAGIC_BYTES; i++) {
        if (raw[i] != (uint8_t)MAGIC[i]) {
            return false;
        }
    }
    if (raw[8] != CATALOG_VERSION) {
        return false;
    }
    out->record_bytes = le16(&raw[9]);
    out->record_count = le16(&raw[11]);
    /* A stride below the fields it must carry would overlap its neighbour, and
     * zero would put every record at the same offset. */
    return out->record_bytes >= RECORD_MINIMUM;
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
}
