/* Font for the freezer UI, generated from ascii00-7f.png by the parent
 * Makefile's tools/pngprepare and checked in beside this file.
 *
 * #embed (C23) resolves it relative to this file, so no assembly stub is
 * needed just to reach .incbin. */

static const unsigned char CHARSET_DATA[] = {
#embed "ascii8x8.bin"
};

/* Non-const to match the extern declaration in monitor/screen.c; the
 * data is only ever read, and lcopy()'d to CHARSET_ADDRESS. */
unsigned char* charset = (unsigned char*)CHARSET_DATA;
