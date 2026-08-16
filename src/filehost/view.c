/* The browser's index of record numbers: which records are shown, in what
 * order, and nothing about what a record holds. */

#include "view.h"

/* Placed by link.ld, above the code and below the screen: two thousand bytes
 * among the C variables would be the largest thing in the region the soft stack
 * grows into.  The host build has no such region and no such script. */
#if defined(__mos__)
#define VIEW_INDEX __attribute__((section(".viewindex")))
#else
#define VIEW_INDEX
#endif

static VIEW_INDEX uint16_t rows[VIEW_MAX];
/* Each row's sort key, read once and carried through the shuffle beside the
 * record number it belongs to. */
static VIEW_INDEX uint8_t keys[VIEW_MAX];
static uint16_t shown;
static uint16_t records;
static enum ViewOrder order;

void view_reset(uint16_t count) {
    /* Only the bound: what is shown and in what order is view_build()'s, and
     * every caller builds next. */
    records = count > VIEW_MAX ? VIEW_MAX : count;
}

uint16_t view_count(void) {
    return shown;
}

enum ViewOrder view_order(void) {
    return order;
}

uint16_t view_row(uint16_t n) {
    return rows[n];
}

void view_build(enum ViewOrder wanted) {
    order = wanted;

    shown = 0;
    for (uint16_t record = 0; record < records; record++) {
        if (view_matches(record)) {
            rows[shown++] = record;
        }
    }
    if (wanted == ViewByTitle) {
        return; /* the file is already in title order */
    }

    /* Every key first, one read of each record and no more: asking again inside
     * the sort would make the far reads quadratic, and those are what the time
     * is spent on. */
    for (uint16_t i = 0; i < shown; i++) {
        keys[i] = view_key(rows[i], wanted);
    }

    /* Insertion sort, which is stable: equal keys keep the order they were
     * added in, and that is the file's own title order.  So "by category"
     * reads as titles grouped under a heading rather than shuffled within it.
     * The shuffle is quadratic, but it moves bytes in memory rather than
     * reading records, which is what VIEW_MAX is sized against. */
    for (uint16_t i = 1; i < shown; i++) {
        const uint16_t record = rows[i];
        const uint8_t key = keys[i];
        uint16_t j = i;
        while (j && keys[j - 1] > key) {
            rows[j] = rows[j - 1];
            keys[j] = keys[j - 1];
            j--;
        }
        rows[j] = record;
        keys[j] = key;
    }
}
