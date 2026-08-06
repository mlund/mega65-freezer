#pragma once

/* Chunked operations over a byte-addressed memory the caller supplies.
 *
 * Host-compilable: no MEGA65 dependency, so a host build can drive these
 * against a flat array the way test/disasm_host_harness.c drives the
 * disassembler.  The two accesses below are the whole platform interface.
 */

#include <stdbool.h>
#include <stdint.h>

/* Supplied by the caller, as disasm.h does for reads. */
extern bool disasm_read_byte(uint32_t address, uint8_t* value);
extern bool block_write_byte(uint32_t address, unsigned char value);

/* Copy `length` bytes, whatever the overlap.  False if an access failed, in
 * which case some of the copy may have happened. */
bool block_move(uint32_t from, uint32_t to, uint32_t length);

/* Read up to a chunk into block_buffer, returning how many bytes arrived.
 * Callers walking two regions read one side in bulk and compare the other a
 * byte at a time, which is what keeps a one-page cache from thrashing. */
constexpr uint16_t BLOCK_MAX = 256;
extern unsigned char block_buffer[BLOCK_MAX];
uint16_t block_read(uint32_t address, uint32_t wanted);
