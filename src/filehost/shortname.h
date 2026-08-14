#pragma once

#include <stdint.h>

/* 8 + '.' + 3 + terminator. */
constexpr uint8_t SHORT_NAME_BYTES = 13;

/* The name a catalogue entry gets on the card, derived from its path with
 * nothing asked of the user.
 *
 * It has to be 8.3: fat32_create_contiguous_file() writes a short directory
 * entry and no long-name ones (src/makedisk/layout.c, fat_name_to_entry), and
 * hyppo's reader keeps only the first long-name block anyway (mega65-core
 * src/hyppo/dos.asm, "2nd and subsequent LFN blocks are not used"), so a longer
 * name is one mega65_dos_attach() may not find again.  The catalogue's own
 * title and path stay on screen; only the card entry is squeezed.
 *
 * Squeezed distinctly: five characters of the name and three of a hash of the
 * whole path, so two catalogue rows cannot land on one card entry.  The path is
 * what is hashed rather than the row number, because a regenerated catalogue
 * reorders the rows and a file already fetched should still be recognised.
 *
 * `kind` picks the extension, so a record always gets the one its type implies
 * rather than whatever the path happened to end in.  `out` holds
 * SHORT_NAME_BYTES. */
void catalog_short_name(const char* path, uint8_t kind, char* out);
