#pragma once

#include <stdint.h>

/* 8 + '.' + 3 + terminator. */
constexpr uint8_t SHORT_NAME_BYTES = 13;

/* The name a catalogue entry gets on the card, derived from its path with
 * nothing asked of the user.  `out` holds SHORT_NAME_BYTES.
 *
 * Must be 8.3: we write no long-name entries, and hyppo's reader keeps only
 * the first long-name block anyway (mega65-core src/hyppo/dos.asm, "2nd and
 * subsequent LFN blocks are not used"), so a longer name is one
 * mega65_dos_attach() may not find again.
 *
 * Five characters of the name and three of a hash of the whole path, so two
 * rows cannot collide.  The path is hashed rather than the row number because
 * a regenerated catalogue reorders rows, and a file already fetched should
 * still be recognised.  `kind` picks the extension. */
void catalog_short_name(const char* path, uint8_t kind, char* out);
