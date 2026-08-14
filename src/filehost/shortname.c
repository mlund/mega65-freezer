#include "shortname.h"

#include "catalog.h"

/* Five characters of the name and three of a hash.  Truncation alone would not
 * do: FileHost's stems carry a random suffix to tell near-identical titles
 * apart, and that suffix is exactly what the first eight characters throw away
 * -- two rows would land on one card entry, and attaching either would mount
 * whichever was written last while the screen named the other. */
static constexpr uint8_t STEM_BYTES = 5;
static constexpr uint8_t HASH_BYTES = 3;

/* Base 32, so each character is a shift and a mask rather than a division. */
static const char HASH_DIGITS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
static constexpr uint8_t HASH_SHIFT = 5;
static constexpr uint8_t HASH_MASK = (1 << HASH_SHIFT) - 1;

/* When the path holds nothing a directory entry can keep.  A name that cannot
 * be attached again is worse than a dull one. */
static const char FALLBACK[] = "FILE";

/* FNV-1a, sixteen bits, over the whole path -- the directories included, since
 * they are part of what makes two entries different. */
static uint16_t path_hash(const char* path) {
    uint16_t hash = 0x811C;
    while (*path) {
        hash ^= (uint8_t)*path++;
        hash *= 0x0193;
    }
    return hash;
}

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* What fat_name_to_entry() packs without changing it, and hyppo matches. */
static bool keepable(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

void catalog_short_name(const char* path, uint8_t kind, char* out) {
    /* The stem runs from the last slash to the *last* dot, so a name carrying
     * dots of its own keeps everything but its extension. */
    const char* base = path;
    const char* stop = nullptr;
    const char* at = path;
    for (; *at; at++) {
        if (*at == '/') {
            base = at + 1;
            stop = nullptr;
        } else if (*at == '.') {
            stop = at;
        }
    }
    if (!stop || stop == base) {
        stop = at;
    }

    uint8_t len = 0;
    for (const char* from = base; from != stop && len < STEM_BYTES; from++) {
        const char c = upper(*from);
        if (keepable(c)) {
            out[len++] = c;
        }
    }
    if (!len) {
        while (FALLBACK[len]) {
            out[len] = FALLBACK[len];
            len++;
        }
    }

    uint16_t hash = path_hash(path);
    for (uint8_t i = 0; i < HASH_BYTES; i++) {
        out[len++] = HASH_DIGITS[hash & HASH_MASK];
        hash >>= HASH_SHIFT;
    }

    out[len++] = '.';
    const char* extension = (kind == CatalogD81) ? "D81" : "PRG";
    for (uint8_t i = 0; i < 3; i++) {
        out[len++] = extension[i];
    }
    out[len] = 0;
}
