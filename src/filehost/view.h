#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Which records the browser shows, and in what order.
 *
 * An index of record numbers, and nothing else: sorting reorders a few hundred
 * two-byte entries where sorting the records themselves would move 33KB, and
 * searching narrows the same index rather than keeping a second list beside it.
 * The browser draws through view_row() and so needs to know neither.
 *
 * It holds no records, which is what keeps it off the machine: what a record
 * contains and what the user typed both arrive through the seams below, so all
 * of this compiles for the host and is tested there. */

/* The catalogue can name more records than the browser shows: it takes the
 * first VIEW_MAX and the count on screen says "N OF M" when that is not all of
 * them.  In practice the buffer runs out first -- it holds 511 records at the
 * format's own stride -- so this is the looser of the two bounds.
 *
 * A round number under two bounds rather than either of them: the region
 * link.ld sets aside would hold 682 rows and their keys, and the sort reads
 * each key once but shuffles them quadratically -- 258 records order in half a
 * second on the machine, and four times as many would not. */
constexpr uint16_t VIEW_MAX = 512;

/* The largest key there is, so whatever a record did not state sorts after
 * everything that did.  Named here because the sort's contract is this
 * module's, and every seam -- including the tests' -- should quote one name. */
constexpr uint8_t VIEW_KEY_UNKNOWN = 0xff;

enum ViewOrder : uint8_t {
    /* The order the file is in, which the format has already sorted by title --
     * so this one costs no sort at all. */
    ViewByTitle,
    ViewByYear,
    ViewByCategory,
};

/* How many records there are to choose from.  Clamped to VIEW_MAX. */
void view_reset(uint16_t records);

/* Rebuilds the index: every record the search accepts, in the order given.
 *
 * One call rather than a filter and a sort to keep in step, because every key
 * that changes what is on screen changes both -- a search reorders nothing but
 * must respect the order in force, and an order applies only to what the search
 * left. */
void view_build(enum ViewOrder order);

/* How many records the index holds, which is how many rows there are to show. */
[[nodiscard]] uint16_t view_count(void);

/* The order the index was last built in, so a search can rebuild without the
 * caller having to remember which one is in force. */
[[nodiscard]] enum ViewOrder view_order(void);

/* The record behind row `n`, which is what the browser draws and attaches.
 * `n` must be below view_count(). */
[[nodiscard]] uint16_t view_row(uint16_t n);

/* Whether a record passes the search in force.  Whoever links this owns the
 * search text and how a title is compared against it. */
[[nodiscard]] bool view_matches(uint16_t record);

/* What to sort a record by, under `order`.  Asked once per record and then
 * kept, because answering it means reading the record out of far memory: asked
 * per comparison instead, a few hundred records cost hundreds of thousands of
 * reads and the machine sits silent for the length of them.
 *
 * A byte, which is what makes keeping them affordable -- so an order with more
 * distinct values than that is one this cannot express, and ranks them instead.
 * Always sorted ascending, so which way round an order reads is decided there
 * rather than here: newest first counts down, and a field the file did not
 * state answers VIEW_KEY_UNKNOWN, which puts it last instead of first. */
[[nodiscard]] uint8_t view_key(uint16_t record, enum ViewOrder order);
