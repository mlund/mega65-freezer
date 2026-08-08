#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The register-name database and the row it renders.  The database is a file on
 * the card rather than an array in the binary, reached only through the
 * caller-supplied iomap_byte() below -- so nothing here touches a register or
 * the screen, and test/verify_bitedit.py builds this file for the host with a
 * buffer standing in for the card.  bitedit.c holds the parts that cannot be. */

/* Supplied by the caller: one byte of IOMAP.BIN, or 0 past the end and when no
 * file is loaded.  A zero there fails the magic check in bitedit_open(), which
 * is how a missing card file becomes a table with nothing in it rather than an
 * error. */
extern uint8_t iomap_byte(uint16_t offset);

/* Column layout of one row.  The bit cells are 6 wide on a 7 stride, so the
 * last ends at 77 and the row fits 80 with two to spare. */
constexpr uint8_t BITEDIT_ADDRESS_WIDTH = 8;
constexpr uint8_t BITEDIT_NAME_WIDTH = 8;
constexpr uint8_t BITEDIT_VALUE_COLUMN = 19;
constexpr uint8_t BITEDIT_BIT_COLUMN = 23;
constexpr uint8_t BITEDIT_BIT_STRIDE = 7;
constexpr uint8_t BITEDIT_BIT_WIDTH = 6;
constexpr uint8_t BITEDIT_ROW_WIDTH = 78;

/* Cursor positions: one per bit, then the value field to their left. */
constexpr uint8_t BITEDIT_CELL_VALUE = 8;
constexpr uint8_t BITEDIT_CELL_COUNT = 9;

/* No chip has been printed yet, so the next row must print its own. */
constexpr uint8_t BITEDIT_NO_CHIP = 0xFF;

constexpr uint8_t BITEDIT_MAX_FIELDS = 8;

/* No description for this register or bit.  Offset 0 cannot be one: the file
 * opens with a header, so nothing real ever lives there. */
constexpr uint16_t BITEDIT_NO_TEXT = 0;

/* One address, decoded.  The fields are offsets into the file rather than
 * pointers, because the table is not in this program's address space. */
typedef struct {
    uint8_t chip;
    uint8_t field_count;
    uint16_t field_name[BITEDIT_MAX_FIELDS];
    uint16_t field_text[BITEDIT_MAX_FIELDS];
    uint16_t name;
    uint16_t text;
    /* Per bit, 7 down to 0: which field names it, or field_count for none. */
    uint8_t bit_field[8];
} RegisterInfo;

/* Read the header and remember where the records and the text begin.  False if
 * the file is absent or is not one of ours, after which every lookup misses. */
bool bitedit_open(void);

/* $D000-$DFFF for an address in any of the four I/O personalities, else 0. */
uint16_t bitedit_io_address(uint32_t address);

/* False when the address has no entry; `out` is cleared either way.
 *
 * `scan` carries the walk's position between calls.  A window is drawn in
 * increasing address order over one sorted table, so sharing a scan across its
 * rows turns fourteen walks from the front into one.  Start it at zero; zero is
 * also correct for a one-off lookup. */
bool bitedit_lookup(uint16_t io_address, RegisterInfo* out, uint16_t* scan);

/* Where a chip's name lives, for callers that want to print it themselves. */
uint16_t bitedit_chip_name(uint8_t index);

/* Copy one stored string out as screen codes, stopping at the terminator or at
 * `limit`.  Returns how many characters were written. */
uint16_t bitedit_string(uint16_t offset, char* out, uint16_t limit);

/* Writes BITEDIT_ROW_WIDTH screen codes.  Returns the chip index the row now
 * establishes, to pass back as `previous_chip` for the row below: the chip is
 * printed only where it changes, and a row carrying its own mnemonic leaves it
 * standing rather than clearing it. */
uint8_t bitedit_render(
    char* out, uint32_t address, uint8_t value, const RegisterInfo* info, uint8_t previous_chip);

/* The dimmed heading above the rows, also BITEDIT_ROW_WIDTH wide. */
void bitedit_header(char* out);

uint8_t bitedit_toggle(uint8_t value, uint8_t bit);
uint8_t bitedit_hex_digit(uint8_t value, uint8_t digit);

/* The cursor ring, one step each way: the value field then bits 7 down to 0,
 * wrapping, so neither end of a row is a key that does nothing.  Here rather
 * than in the edit loop because the host test can reach it here -- the emulator
 * cannot press a shifted cursor key, so left is otherwise never exercised. */
uint8_t bitedit_cursor_left(uint8_t cell);
uint8_t bitedit_cursor_right(uint8_t cell);

/* How many characters of `body` from `at` belong on a line `room` wide: a whole
 * word boundary where one fits, the whole remainder where it fits, and a hard
 * cut only for a word wider than the row.  Here rather than in the drawing code
 * because the emulator is an expensive place to find an off-by-one -- a wrap
 * that broke "RASTER COMPARE BIT 8" across two lines survived to a screenshot
 * before anything noticed. */
uint8_t bitedit_wrap(const char* body, uint16_t length, uint16_t at, uint8_t room);
