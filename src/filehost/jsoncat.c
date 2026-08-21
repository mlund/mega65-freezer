// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
/* Scanning JSON without holding any of it, and packing the seven fields worth
 * keeping into the fixed-width record the browser reads. */

#include "jsoncat.h"

#include "catalog.h"
#include "format.h"

#include <string.h>

/* Offsets within a record, quoted from ether65 docs/FILEHOST.md section 2 --
 * the same numbers catalog.c reads them back at, stated separately on purpose
 * so a change to one is not silently a change to the other. */
static constexpr uint8_t RECORD_TITLE = 0;
static constexpr uint8_t RECORD_AUTHOR = 40;
static constexpr uint8_t RECORD_PATH = 56;
static constexpr uint8_t RECORD_KIND = 104;
static constexpr uint8_t RECORD_SIZE = 105;
static constexpr uint8_t RECORD_CATEGORY = 109;
static constexpr uint8_t RECORD_YEAR = 110;

/* The header the file starts with. */
static constexpr uint8_t MAGIC_BYTES = 8;
static const char MAGIC[MAGIC_BYTES] = {'M', '6', '5', 'F', 'H', 'C', 'A', 'T'};
/* Version 2, which is the one with a category and a year in it -- both of
 * which upstream states, so there is no reason to write the older shape. */
static constexpr uint8_t CATALOG_VERSION = 2;

/* Which of upstream's keys a value belongs to.  Not the field's offset,
 * because two of them need work before they are one. */
enum Field : uint8_t {
    FieldNone,
    FieldTitle,
    FieldAuthor,
    FieldLocation,
    FieldFilename,
    FieldSize,
    FieldCategory,
    FieldPublished,
};

/* Two bytes of the file, which is little-endian throughout. */
static void put_le16(uint8_t* at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t* at, uint32_t value) {
    put_le16(at, (uint16_t)value);
    put_le16(at + 2, (uint16_t)(value >> 16));
}

/* Whether two strings are the same, ignoring case: letters differ only in bit
 * 5, and every name compared here is letters. */
[[nodiscard]] static bool same_text(const char* a, const char* b) {
    for (; *a; a++, b++) {
        if ((*a | 0x20) != (*b | 0x20)) {
            return false;
        }
    }
    return !*b;
}

/* Copies at most `width` characters and pads the rest, which is how the format
 * carries a string in a fixed field: spaces for the two text fields, and NULs
 * for the path, exactly as catalog.c strips them back off. */
static void put_field(uint8_t* at, const char* text, uint8_t width, uint8_t pad) {
    uint8_t i = 0;
    for (; i < width && text[i]; i++) {
        at[i] = (uint8_t)text[i];
    }
    for (; i < width; i++) {
        at[i] = pad;
    }
}

/* Where the path begins in a location, which arrives as a whole URL.
 *
 * Anchored on the scheme's own "://" rather than on the first "//" anywhere,
 * so a path that happens to contain a doubled slash is not mistaken for a
 * host.  A location with no scheme is already a path and is taken as it
 * stands, which is what a proxy rewriting them that way would send. */
[[nodiscard]] static const char* url_path(const char* text) {
    for (const char* at = text; *at; at++) {
        if (at[0] == ':' && at[1] == '/' && at[2] == '/') {
            at += 3;
            while (*at && *at != '/') {
                at++;
            }
            return at;
        }
    }
    return text;
}

/* The kind behind a filename's extension, and false for one this browser
 * cannot open -- which is most of them: upstream carries .zip, .7z, .pdf and
 * more, and the gateway drops those today for the same reason. */
[[nodiscard]] static bool kind_of(const char* filename, uint8_t* out) {
    const char* extension = "";
    for (const char* at = filename; *at; at++) {
        if (*at == '.') {
            extension = at + 1;
        }
    }
    if (same_text(extension, "d81")) {
        *out = CatalogD81;
        return true;
    }
    if (same_text(extension, "prg")) {
        *out = CatalogPrg;
        return true;
    }
    return false;
}

/* The category behind upstream's own word for it.
 *
 * Compared against catalog.c's own table rather than a second list of the same
 * seven words: written out again here, the two would agree until the day
 * somebody added an eighth to one of them. */
[[nodiscard]] static uint8_t category_of(const char* name) {
    for (uint8_t category = CatalogGame; category <= CatalogFirmware; category++) {
        if (same_text(name, catalog_category_name(category))) {
            return category;
        }
    }
    return CatalogNoCategory;
}

/* Which field the key just read names, and FieldNone for the ten keys upstream
 * sends that this does not read. */
[[nodiscard]] static uint8_t field_of(const char* key) {
    if (same_text(key, "title")) {
        return FieldTitle;
    }
    if (same_text(key, "author")) {
        return FieldAuthor;
    }
    if (same_text(key, "location")) {
        return FieldLocation;
    }
    if (same_text(key, "filename")) {
        return FieldFilename;
    }
    if (same_text(key, "size")) {
        return FieldSize;
    }
    if (same_text(key, "category")) {
        return FieldCategory;
    }
    if (same_text(key, "published")) {
        return FieldPublished;
    }
    return FieldNone;
}

/* A value complete, put where it belongs. */
static void commit(struct JsonCatalog* catalog) {
    const char* value = catalog->value;
    switch (catalog->field) {
        case FieldTitle:
            put_field(&catalog->record[RECORD_TITLE], value, CATALOG_TITLE_BYTES, ' ');
            break;
        case FieldAuthor:
            put_field(&catalog->record[RECORD_AUTHOR], value, CATALOG_AUTHOR_BYTES, ' ');
            break;
        case FieldLocation: {
            /* A value that did not fit the buffer cannot be judged: a path cut
             * short is a plausible length and a different file.  The record
             * goes rather than the path being trimmed to fit. */
            if (catalog->overflowed) {
                break;
            }
            const char* path = url_path(value);
            const uint8_t length = (uint8_t)strlen(path);
            if (!length || length > CATALOG_PATH_BYTES) {
                break;
            }
            put_field(&catalog->record[RECORD_PATH], path, CATALOG_PATH_BYTES, 0);
            catalog->has_path = true;
            break;
        }
        case FieldFilename: {
            uint8_t kind;
            if (kind_of(value, &kind)) {
                catalog->record[RECORD_KIND] = kind;
                catalog->has_kind = true;
            }
            break;
        }
        case FieldSize:
            /* Upstream quotes its numbers today and need not always; the
             * scanner hands over either shape, and this reads both. */
            put_le32(&catalog->record[RECORD_SIZE], parse_dec(value));
            break;
        case FieldCategory:
            catalog->record[RECORD_CATEGORY] = category_of(value);
            break;
        case FieldPublished:
            /* YYYY/MM/DD, the only shape all 454 records use.  parse_dec() stops
             * at the slash, so the year is what it returns. */
            put_le16(&catalog->record[RECORD_YEAR], (uint16_t)parse_dec(value));
            break;
        default:
            break;
    }
}

/* A record beginning: everything cleared, because a key upstream did not send
 * on this record would otherwise keep the last one's answer. */
static void begin_record(struct JsonCatalog* catalog) {
    /* Title and author are adjacent and both space-padded, so the two are one
     * fill; everything after them is NUL-padded or a number. */
    memset(&catalog->record[RECORD_TITLE], ' ', RECORD_PATH);
    memset(&catalog->record[RECORD_PATH], 0, JSONCAT_RECORD_BYTES - RECORD_PATH);
    catalog->has_kind = false;
    catalog->has_path = false;
    catalog->field = FieldNone;
}

/* A record ending: kept only if it has the two things it cannot do without. */
static void finish_record(struct JsonCatalog* catalog) {
    if (catalog->has_kind && catalog->has_path) {
        catalog->count++;
        catalog->ready = true;
    } else {
        catalog->skipped++;
    }
}

/* A token beginning, quoted or not. */
static void begin_token(struct JsonCatalog* catalog) {
    catalog->collecting = true;
    catalog->value_length = 0;
    catalog->overflowed = false;
    /* A key is always collected -- it is what says whether its value is worth
     * collecting.  A value is collected only if its key named one of the seven
     * fields, and nothing at all is collected below a record's own level. */
    catalog->wanted =
        catalog->depth == 1 && (!catalog->expect_value || catalog->field != FieldNone);
}

static void append(struct JsonCatalog* catalog, uint8_t byte) {
    if (!catalog->wanted) {
        return;
    }
    if (catalog->value_length < JSONCAT_VALUE_MAX) {
        catalog->value[catalog->value_length++] = (char)byte;
    } else {
        catalog->overflowed = true;
    }
}

/* A token ending: a key says which field the value after it belongs to, a
 * value is put in that field, and anything below a record's own level is
 * neither. */
static void end_token(struct JsonCatalog* catalog) {
    if (!catalog->collecting) {
        return;
    }
    catalog->collecting = false;
    catalog->value[catalog->value_length] = 0;
    if (catalog->depth != 1) {
        return;
    }
    if (catalog->expect_value) {
        commit(catalog);
        catalog->expect_value = false;
    } else {
        catalog->field = field_of(catalog->value);
    }
}

/* One byte of the reply.
 *
 * Strings first, and entirely: inside one, every character is text.  A brace
 * counted as structure because it happened to be inside a title ends the
 * record early and puts every record after it out of step, and an escaped
 * quote read as a real one does the same -- upstream sends both, in titles and
 * in the HTML it puts in `hiddensize`.
 *
 * An escaped character is taken literally rather than translated.  The two
 * that occur are \" and \\, which are already themselves; nothing in the seven
 * fields this reads has ever carried \n or \u. */
static void feed(struct JsonCatalog* catalog, uint8_t byte) {
    if (catalog->in_string) {
        if (catalog->escaped) {
            catalog->escaped = false;
        } else if (byte == '\\') {
            catalog->escaped = true;
            return;
        } else if (byte == '"') {
            catalog->in_string = false;
            end_token(catalog);
            return;
        }
        append(catalog, byte);
        return;
    }

    switch (byte) {
        case '"':
            begin_token(catalog);
            catalog->in_string = true;
            return;
        case ':':
            catalog->expect_value = true;
            return;
        case ',':
            end_token(catalog);
            catalog->expect_value = false;
            return;
        case '{':
            end_token(catalog);
            catalog->depth++;
            if (catalog->depth == 1) {
                begin_record(catalog);
            }
            return;
        case '}':
            end_token(catalog);
            if (catalog->depth == 1) {
                finish_record(catalog);
            }
            if (catalog->depth) {
                catalog->depth--;
            }
            catalog->expect_value = false;
            return;
        case '[':
        case ']':
            end_token(catalog);
            return;
        default:
            if (byte <= ' ') {
                end_token(catalog); /* whitespace ends an unquoted number */
                return;
            }
            /* An unquoted value -- a number, or true/false/null.  Keys are
             * always quoted, so nothing else is collected here. */
            if (catalog->expect_value) {
                if (!catalog->collecting) {
                    begin_token(catalog);
                }
                append(catalog, byte);
            }
            return;
    }
}

void jsoncat_begin(struct JsonCatalog* catalog) {
    *catalog = (struct JsonCatalog){0};
}

bool jsoncat_take(struct JsonCatalog* catalog,
    const uint8_t* bytes,
    uint16_t length,
    uint16_t* taken,
    uint8_t* record_out) {
    uint16_t at = 0;
    while (at < length) {
        feed(catalog, bytes[at++]);
        if (catalog->ready) {
            catalog->ready = false;
            memcpy(record_out, catalog->record, JSONCAT_RECORD_BYTES);
            *taken = at;
            return true;
        }
    }
    *taken = at;
    return false;
}

bool jsoncat_end(struct JsonCatalog* catalog, uint8_t* header_out) {
    memset(header_out, 0, CATALOG_HEADER_BYTES);
    memcpy(header_out, MAGIC, MAGIC_BYTES);
    header_out[8] = CATALOG_VERSION;
    put_le16(&header_out[9], JSONCAT_RECORD_BYTES);
    put_le16(&header_out[11], catalog->count);
    /* Where a JSON array should end, and where a connection cut part way does
     * not.  The catalogue endpoint states no length, so its body ends at the
     * close -- which means a truncated reply is indistinguishable from a whole
     * one unless the shape of the JSON says otherwise. */
    return catalog->depth == 0 && !catalog->in_string;
}
