/*
 * Exercises this project's own lcopy() and lfill() -- the DMAgic-driven ones in
 * src/dma.c.  Nothing on the host can reach them: they populate a DMA
 * list and hand it to hardware, so they need a machine or an emulator.
 *
 * Run by CTest under Xemu.  Each check has a number and failure quits the
 * emulator with it, so the exit status says which one went, not merely that one
 * did.  Zero means every check below passed.
 *
 * <mega65/tests.h> is deliberately not used, though it offers assert_eq: it
 * references lfill and lpoke, so linking it pulls in mega65-libc's memory.c,
 * whose lcopy/lfill/dma_byte collide with the ones under test.  Quitting the
 * emulator is three lines, so it is done here instead.
 *
 * The scratch area is colour RAM.  Xemu returns stale data for DMA reads from
 * low 28-bit addresses -- $008000 and $040000 both -- where $FF80000 behaves
 * the same in the emulator and on hardware.
 */

#include "dma.h"

#include <mega65/debug.h>

/* Writing a code here and then XEMU_QUIT ends the run with that code.  Needs
 * Xemu's -testing; on hardware the write lands in an unused register and the
 * program simply carries on. */
constexpr uint16_t XEMU_CONTROL = 0xD6CF;
constexpr uint8_t XEMU_QUIT = 0x42;

[[noreturn]] static void finish(uint8_t code) {
    POKE(XEMU_CONTROL, code);
    POKE(XEMU_CONTROL, XEMU_QUIT);
    for (;;) {
        __asm__ volatile("");
    }
}

static void check(uint8_t number, uint8_t got, uint8_t want) {
    if (got != want) {
        debug_msg("FAIL");
        finish(number);
    }
}

/* Colour RAM is 32K at $FF80000; the screen uses the first 2000 bytes, so this
 * is well clear of it. */
constexpr Addr28 SCRATCH = 0xFF84000L;
constexpr Addr28 SCRATCH2 = 0xFF85000L;
constexpr uint16_t SPAN = 512;

int main(void) {
    /* Ours, from dma.h.  <mega65/memory.h> is deliberately not
     * included either: it declares lcopy/lfill with uint32_t where
     * dma.c defines them with Addr28. */
    m65_io_enable();

    debug_msg("TEST: lfill");
    lfill(SCRATCH, 0xA5, SPAN);
    check(1, lpeek(SCRATCH), 0xA5);
    check(2, lpeek(SCRATCH + SPAN / 2), 0xA5);
    check(3, lpeek(SCRATCH + SPAN - 1), 0xA5);

    /* The byte past the end must be untouched, which is what catches an
     * off-by-one in the count. */
    debug_msg("TEST: lfill stops at count");
    lfill(SCRATCH + SPAN, 0x3C, 1);
    lfill(SCRATCH, 0x5A, SPAN);
    check(4, lpeek(SCRATCH + SPAN - 1), 0x5A);
    check(5, lpeek(SCRATCH + SPAN), 0x3C);

    debug_msg("TEST: lcopy");
    lfill(SCRATCH2, 0x00, SPAN);
    lcopy(SCRATCH, SCRATCH2, SPAN);
    check(6, lpeek(SCRATCH2), 0x5A);
    check(7, lpeek(SCRATCH2 + SPAN - 1), 0x5A);

    /* A distinguishable pattern, so a copy that moves the right number of the
     * wrong bytes still fails. */
    debug_msg("TEST: lcopy moves the right bytes");
    lpoke(SCRATCH, 0x11);
    lpoke(SCRATCH + 1, 0x22);
    lpoke(SCRATCH + SPAN - 1, 0x33);
    lcopy(SCRATCH, SCRATCH2, SPAN);
    check(8, lpeek(SCRATCH2), 0x11);
    check(9, lpeek(SCRATCH2 + 1), 0x22);
    check(10, lpeek(SCRATCH2 + SPAN - 1), 0x33);

    /* Both take an early return on a zero count, so the destination must not
     * move. */
    debug_msg("TEST: zero count is a no-op");
    lpoke(SCRATCH2, 0x77);
    lcopy(SCRATCH, SCRATCH2, 0);
    check(11, lpeek(SCRATCH2), 0x77);
    lfill(SCRATCH2, 0xEE, 0);
    check(12, lpeek(SCRATCH2), 0x77);

    /* The strided pair, which writes every skip'th byte.  Filling with a stride
     * of 2 must leave the odd bytes alone, which is exactly what a colour plane
     * in the 16-bit text mode needs. */
    debug_msg("TEST: lfill_skip");
    lfill(SCRATCH2, 0x00, SPAN);
    lfill_skip(SCRATCH2, 0xC3, 4, 2);
    check(13, lpeek(SCRATCH2 + 0), 0xC3);
    check(14, lpeek(SCRATCH2 + 1), 0x00);
    check(15, lpeek(SCRATCH2 + 2), 0xC3);
    check(16, lpeek(SCRATCH2 + 6), 0xC3);
    check(17, lpeek(SCRATCH2 + 8), 0x00); /* count is bytes written, not spanned */

    debug_msg("TEST: lcopy_skip");
    lpoke(SCRATCH + 0, 0x11);
    lpoke(SCRATCH + 1, 0x22);
    lpoke(SCRATCH + 2, 0x33);
    lfill(SCRATCH2, 0x00, SPAN);
    lcopy_skip(SCRATCH, SCRATCH2, 3, 2);
    check(18, lpeek(SCRATCH2 + 0), 0x11);
    check(19, lpeek(SCRATCH2 + 1), 0x00);
    check(20, lpeek(SCRATCH2 + 2), 0x22);
    check(21, lpeek(SCRATCH2 + 4), 0x33);

    /* The claim dma.h rests on: the DMAgic restores a step of one at the end of
     * every chain, so a strided job cannot leak into the next.  If it did, this
     * plain fill would scatter every second byte and leave the gaps at 0xC3. */
    debug_msg("TEST: the stride does not leak into the next job");
    lfill(SCRATCH2, 0xC3, SPAN);
    lfill_skip(SCRATCH2, 0x5A, 4, 2);
    lfill(SCRATCH2, 0x7E, 8);
    check(22, lpeek(SCRATCH2 + 0), 0x7E);
    check(23, lpeek(SCRATCH2 + 1), 0x7E);
    check(24, lpeek(SCRATCH2 + 7), 0x7E);

    debug_msg("TEST: strided zero count is a no-op");
    lpoke(SCRATCH2, 0x99);
    lfill_skip(SCRATCH2, 0x00, 0, 2);
    check(25, lpeek(SCRATCH2), 0x99);
    lcopy_skip(SCRATCH, SCRATCH2, 0, 2);
    check(26, lpeek(SCRATCH2), 0x99);

    debug_msg("PASS");
    finish(0);
}
