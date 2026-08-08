#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The register-name database and the row it renders.  Nothing here touches a
 * register or the screen: every function reads the static table and writes into
 * a buffer the caller owns, which is what lets test/verify_bitedit.py compile
 * this file for the host.  bitedit.c holds the parts that cannot be. */

/* Column layout of one row.  The bit cells are 6 wide on a 7 stride, so the
 * last ends at 77 and the row fits 80 with two to spare. */
constexpr uint8_t BITEDIT_ADDRESS_WIDTH = 8;
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

/* One address, decoded.  `field` entries point into the table and are screen
 * codes with bit 7 set on the last character, not C strings. */
typedef struct {
    uint8_t chip;
    uint8_t field_count;
    const uint8_t* field[BITEDIT_MAX_FIELDS];
    const uint8_t* name;
    /* Per bit, 7 down to 0: which field names it, or field_count for none. */
    uint8_t bit_field[8];
} RegisterInfo;

/* $D000-$DFFF for an address in any of the four I/O personalities, else 0. */
uint16_t bitedit_io_address(uint32_t address);

/* False when the address has no entry; `out` is cleared either way.
 *
 * `scan` carries the walk's position between calls.  A window is drawn in
 * increasing address order over one sorted table, so sharing a scan across its
 * rows turns sixteen walks from the front into one -- measured at 4817 record
 * steps per repaint against 316.  Start it at zero; zero is also correct for a
 * one-off lookup. */
bool bitedit_lookup(uint16_t io_address, RegisterInfo* out, uint16_t* scan);

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
