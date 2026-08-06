#pragma once

/* A trace line on the hypervisor's serial channel, for the failure paths.
 *
 * debug_msg() writes each character to $D643, which is a hypervisor trap rather
 * than an emulator hook, so the same line appears on a real machine over serial
 * as it does on Xemu's stdout under -hyperserialascii.
 *
 * Off by default: a shipping build should carry no strings for failures that do
 * not happen.  Turn it on with `cmake -DFREEZER_TRACE=ON`, and expect the text
 * to cost .rodata against a budget of 34817 bytes per target.
 *
 * Only the reporting is conditional.  The error returns themselves are always
 * there, so a caller behaves the same either way. */

#ifdef FREEZER_TRACE

#include <mega65/debug.h>

#define TRACE(msg) debug_msg(msg)

#else

/* Consumes the argument so an unused string does not warn, and evaluates to
 * nothing so the call disappears entirely. */
#define TRACE(msg) ((void)sizeof(msg))

#endif
