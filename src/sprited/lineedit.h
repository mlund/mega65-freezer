#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One keystroke of a line being typed, with no screen and no keyboard: the
 * caller owns both.  That is what lets the awkward parts -- backspacing an
 * empty line, filling the last cell, what a control code does -- be driven
 * from the host instead of through the emulator a character at a time.
 *
 * `length` is the caller's, so a redraw between keys sees the same state the
 * edit does.  The buffer stays NUL-terminated at every step.
 *
 * Returns true when RETURN has ended the line. */
bool line_edit(char* buffer, uint8_t capacity, uint8_t* length, uint8_t key);
