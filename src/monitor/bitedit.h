#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The `B` command's screen and keyboard half.  The table it draws from lives in
 * bitedit_table.c, which has no MEGA65 dependency and is tested on the host.
 *
 * Starts at mon_address and returns false if an address was not frozen, leaving
 * the caller to report it and to flush -- the same division the other commands
 * that walk frozen memory use.
 */

extern uint32_t mon_address;

bool edit_bits(void);
