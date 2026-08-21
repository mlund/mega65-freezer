// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The FileHost catalogue as JSON, turned into the fixed-width one the browser
 * already reads: bytes in, 128-byte records out.
 *
 * The proxy mirrors upstream's JSON as it stands, so the machine is what makes
 * it indexable.  What comes out is ether65 docs/FILEHOST.md section 2, so
 * catalog.c, view.c and the browser need not know a proxy exists.
 *
 * Streaming: the catalogue is 292KB and there is nowhere to put it, so the
 * reply is consumed a segment at a time and never exists whole.  Nothing here
 * allocates or seeks, so it compiles for the host and is tested there -- a
 * field taken from the wrong key is plausible wrong entries, not a crash. */

/* The stride this writes.  A reader takes it from the header rather than
 * assuming it, so it is this writer's choice; 128 is what upstream's own
 * transcoder emits and what every catalogue on a card already has. */
constexpr uint8_t JSONCAT_RECORD_BYTES = 128;

/* How much of one JSON value is kept.
 *
 * Sized by the longest value that has to be seen *whole* rather than by the
 * longest field: `location` arrives as a full URL, so the buffer must hold the
 * proxy's scheme and host as well as the path -- measured at 33 characters of
 * prefix against a 48-character path field.  Titles are longer than this and
 * do not need it, being cut to 40 either way.  A value that overflows loses
 * the record, which is the safe way round: a path judged short enough on a
 * truncated copy would name a different file. */
constexpr uint8_t JSONCAT_VALUE_MAX = 96;

/* The transcoder's state.  A caller keeps one for the whole reply.
 *
 * `count` is the records written so far, which is what the header needs and
 * cannot be known until the end.  `skipped` is the records dropped for having
 * no kind this browser can open -- a .zip, a .pdf -- or a path too long for
 * the format's field, which is the same thing that happens today: 195 of
 * upstream's 454 records have no CatalogKind at all. */
struct JsonCatalog {
    uint16_t count;
    uint16_t skipped;
    uint8_t record[JSONCAT_RECORD_BYTES];
    char value[JSONCAT_VALUE_MAX + 1];
    uint8_t value_length;
    /* Which field the value now arriving belongs to, resolved once when its
     * key ended.  The key itself is not kept: nothing needs the word, only
     * which of the seven fields it named. */
    uint8_t field;
    uint8_t depth;
    bool in_string;
    bool escaped;
    bool collecting;
    bool expect_value;
    bool overflowed;
    /* Whether the token being collected is worth the bytes.  Ten of
     * upstream's seventeen keys are never read and their values are 43% of
     * the catalogue -- `hiddensize` alone carries a span of HTML per record --
     * so they are stepped over rather than copied into `value`. */
    bool wanted;
    /* Whether the record being built has the two things it cannot do
     * without: a kind, and a path that fits the field. */
    bool has_kind;
    bool has_path;
    /* Set when a record is complete and waiting to be taken away. */
    bool ready;
};

/* Starts a catalogue.  A caller writes the records this yields from
 * CATALOG_HEADER_BYTES onwards and comes back for the header at the end. */
void jsoncat_begin(struct JsonCatalog* catalog);

/* Feeds up to `length` bytes of the reply, and says through `taken` how many
 * were used.
 *
 * True means a record was completed into `record_out`, JSONCAT_RECORD_BYTES of
 * it; the caller stores it and calls again with the bytes that were not taken.
 * False means the whole of `length` was consumed and no record finished.
 *
 * Two answers rather than a count of records, because the alternative cannot
 * be sized: the caller would have to offer room for the most records the given
 * bytes could possibly produce, and the smallest object that yields one is
 * short enough that a single segment could hold seventy of them.  One record
 * at a time bounds the output at 128 bytes whatever arrives. */
[[nodiscard]] bool jsoncat_take(struct JsonCatalog* catalog,
    const uint8_t* bytes,
    uint16_t length,
    uint16_t* taken,
    uint8_t* record_out);

/* Fills `header_out` with the CATALOG_HEADER_BYTES the file starts with, now
 * that the record count is known.
 *
 * False when the reply did not end where a JSON array should -- a connection
 * cut part way, or a proxy answering with something else -- in which case the
 * header still describes what was written, but the caller should say the
 * catalogue is incomplete rather than store it over a good one. */
[[nodiscard]] bool jsoncat_end(struct JsonCatalog* catalog, uint8_t* header_out);
