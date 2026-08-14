#pragma once

#include "catalog.h"
#include "screen.h"

#include <stdint.h>

/* The browser's layout, in one place: menu.cpp writes the headings at compile
 * time and main.c writes the values under them at run time, and a heading over
 * the wrong column is exactly the drift a shared constant prevents.
 *
 * Eighty columns, so a 40-character title and a 16-character author both fit
 * whole -- at 40 the title would be the only thing on the row.
 *
 * Each column is placed one cell past the end of the one before, and each field
 * runs to the start of the next, so the four tile the row: a highlighted
 * selection is then an unbroken bar rather than four lit runs with gaps
 * between them. */
constexpr uint8_t COLUMN_TITLE = 0;
constexpr uint8_t COLUMN_AUTHOR = COLUMN_TITLE + CATALOG_TITLE_BYTES + 1;
constexpr uint8_t COLUMN_KIND = COLUMN_AUTHOR + CATALOG_AUTHOR_BYTES + 1;
constexpr uint8_t COLUMN_SIZE = COLUMN_KIND + 4;

constexpr uint8_t WIDTH_TITLE = COLUMN_AUTHOR - COLUMN_TITLE;
constexpr uint8_t WIDTH_AUTHOR = COLUMN_KIND - COLUMN_AUTHOR;
constexpr uint8_t WIDTH_KIND = COLUMN_SIZE - COLUMN_KIND;
constexpr uint8_t WIDTH_SIZE = SCREEN_COLS - COLUMN_SIZE;

constexpr uint8_t COUNT_X = 60;
constexpr uint8_t COUNT_Y = 0;
constexpr uint8_t HEADING_Y = 1;
constexpr uint8_t LIST_TOP_Y = 3;
constexpr uint8_t STATUS_Y = 23;
constexpr uint8_t FOOTER_Y = 24;
/* Everything between the rule under the headings and the rule above the
 * status line.  Derived, because a list one row too long would draw over the
 * rule and nothing else would say so. */
constexpr uint8_t LIST_ROWS = STATUS_Y - 1 - LIST_TOP_Y;

#ifdef __cplusplus
extern "C" {
#endif

/* Built by filehost/menu.cpp.  Declared here rather than in main.c so that the
 * definition is checked against the use -- freezer and audiomix each declare
 * their own in the .c because they have no header to share. */
const uint8_t* menu_fixed_stream(void);

#ifdef __cplusplus
}
#endif
