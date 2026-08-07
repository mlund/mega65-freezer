/*
 * Exercises snprintf's decimal conversions on the target.
 *
 * MEGAINFO formats hex everywhere -- %02X, %04X -- and every decimal it shows
 * went through a hand-written itoa.  A decimal conversion therefore has no
 * caller in any of the seven tools, so nothing has ever run this path on the
 * machine.  Hex needs shifts and masks; decimal needs division by ten, which is
 * a different route through the library.
 *
 * Run by CTest under Xemu.  Each check has a number and failure quits the
 * emulator with it, so the exit status says which conversion went wrong.  Zero
 * means every check below passed.  See test_dma.c for the same arrangement.
 */

#include <mega65/debug.h>
#include <peekpoke.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

constexpr uint16_t XEMU_CONTROL = 0xD6CF;
constexpr uint8_t XEMU_QUIT = 0x42;

[[noreturn]] static void finish(uint8_t code) {
    POKE(XEMU_CONTROL, code);
    POKE(XEMU_CONTROL, XEMU_QUIT);
    for (;;) {
        __asm__ volatile("");
    }
}

/* Zero-page pressure, to stand in for a real tool's.  MEGAINFO fills the
 * window from $22 to $F6; a bare test leaves most of it free, and the LTO
 * backend compiles the libc -- printf included -- against whatever is left.
 * Set ZP_FILLER on the command line to sweep. */
#ifndef ZP_FILLER
#define ZP_FILLER 0
#endif
#if ZP_FILLER > 0
__attribute__((section(".zp.bss"))) volatile uint8_t zp_filler[ZP_FILLER];
#endif

static char out[64];

static void check(uint8_t number, const char* want) {
    if (strcmp(out, want) != 0) {
        debug_msg("FAIL: wanted then got");
        debug_msg((char*)want);
        debug_msg(out);
        finish(number);
    }
}

int main(void) {
    /* debug_msg traps to $D643, which needs C65 I/O gated on first. */
    POKE(0xD02FU, 0x47);
    POKE(0xD02FU, 0x53);

#if ZP_FILLER > 0
    zp_filler[0] = 1; /* keep it live */
#endif
    debug_msg("TEST: snprintf hex, which the tools already use");
    snprintf(out, sizeof out, "%02X", 0x5Au);
    check(1, "5A");
    snprintf(out, sizeof out, "%04X", 0x1234u);
    check(2, "1234");

    debug_msg("TEST: snprintf decimal, which nothing has called");
    snprintf(out, sizeof out, "%u", 0u);
    check(3, "0");
    snprintf(out, sizeof out, "%u", 7u);
    check(4, "7");
    snprintf(out, sizeof out, "%u", 42u);
    check(5, "42");
    snprintf(out, sizeof out, "%u", 65535u);
    check(6, "65535");

    debug_msg("TEST: zero-padded decimal, as a date needs");
    snprintf(out, sizeof out, "%02u", 3u);
    check(7, "03");
    snprintf(out, sizeof out, "%02u", 12u);
    check(8, "12");

    /* The two conversions MEGAINFO's date and version lines need. */
    debug_msg("TEST: the composed forms");
    snprintf(out, sizeof out, "%u-%02u-%02u", 2024u, 3u, 7u);
    check(9, "2024-03-07");
    snprintf(out, sizeof out, "%u.%u / %u.%u", 1u, 2u, 3u, 4u);
    check(10, "1.2 / 3.4");

    debug_msg("PASS");
    finish(0);
}
