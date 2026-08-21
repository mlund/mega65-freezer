/* Where the browser puts things on screen: the columns, the rows the list
 * occupies, and the lines above and below it. */
#pragma once

#include "catalog.h"
#include "screen.h"

#include <stdint.h>

/* The browser's layout, in one place: menu.cpp writes the headings at compile
 * time and main.c the values at run time, and a heading over the wrong column
 * is the drift a shared constant prevents.
 *
 * Eighty columns, so a 40-character title and a 16-character author both fit.
 * Each column sits one cell past the last and each field runs to the next, so
 * the five tile the row and a selection is an unbroken bar. */
/* The longest category name, which is what the column has to hold: a heading
 * narrower than its values would be one that lies. */
constexpr uint8_t CATALOG_CATEGORY_BYTES = 11;
/* Four figures.  A year outside them is not one this browser will meet. */
constexpr uint8_t CATALOG_YEAR_BYTES = 4;

constexpr uint8_t COLUMN_TITLE = 0;
constexpr uint8_t COLUMN_AUTHOR = COLUMN_TITLE + CATALOG_TITLE_BYTES + 1;
constexpr uint8_t COLUMN_CATEGORY = COLUMN_AUTHOR + CATALOG_AUTHOR_BYTES + 1;
constexpr uint8_t COLUMN_YEAR = COLUMN_CATEGORY + CATALOG_CATEGORY_BYTES + 1;
constexpr uint8_t COLUMN_KIND = COLUMN_YEAR + CATALOG_YEAR_BYTES + 1;

constexpr uint8_t WIDTH_TITLE = COLUMN_AUTHOR - COLUMN_TITLE;
constexpr uint8_t WIDTH_AUTHOR = COLUMN_CATEGORY - COLUMN_AUTHOR;
constexpr uint8_t WIDTH_CATEGORY = COLUMN_YEAR - COLUMN_CATEGORY;
constexpr uint8_t WIDTH_YEAR = COLUMN_KIND - COLUMN_YEAR;
constexpr uint8_t WIDTH_KIND = SCREEN_COLS - COLUMN_KIND;
static_assert(WIDTH_KIND >= 3, "the type column has no room for D81");

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
