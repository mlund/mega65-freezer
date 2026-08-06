/* Host-compilable -- see blockmove.h. */

#include "blockmove.h"

/* .bss costs nothing in the image. */
unsigned char block_buffer[BLOCK_MAX];

/* As much of what is left as block_buffer holds. */
static uint16_t chunk_size(uint32_t remaining) {
    return (remaining > BLOCK_MAX) ? BLOCK_MAX : (uint16_t)remaining;
}

uint16_t block_read(uint32_t address, uint32_t wanted) {
    uint16_t take = chunk_size(wanted);
    uint16_t got = 0;

    while (got < take && disasm_read_byte(address + got, &block_buffer[got])) {
        got++;
    }
    return got;
}

bool block_move(uint32_t from, uint32_t to, uint32_t length) {
    /* Move the chunk nearest the overlap last: forwards into a destination
     * inside the range would overwrite source bytes still unread. */
    bool backwards = to > from;
    uint32_t remaining = length;
    uint32_t done = 0;

    while (remaining > 0) {
        uint16_t chunk = chunk_size(remaining);
        remaining -= chunk;
        /* The two countdowns are the two offsets: remaining is the trailing
         * chunk's, done the leading one's. */
        uint32_t offset = backwards ? remaining : done;

        if (block_read(from + offset, chunk) < chunk) {
            return false;
        }
        for (uint16_t i = 0; i < chunk; i++) {
            if (!block_write_byte(to + offset + i, block_buffer[i])) {
                return false;
            }
        }
        done += chunk;
    }
    return true;
}
