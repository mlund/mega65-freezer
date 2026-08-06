/* cc65 extensions absent from the llvm-mos libc. */

#include "cc65compat.h"

char* itoa(int value, char* s, int radix) {
    char* p = s;
    unsigned int u;

    if (radix == 10 && value < 0) {
        *p++ = '-';
        u = (unsigned int)-value;
    } else {
        u = (unsigned int)value;
    }

    utoa(u, p, radix);
    return s;
}

char* utoa(unsigned value, char* s, int radix) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buf[17];
    unsigned char n = 0;
    char* p = s;

    do {
        buf[n++] = digits[value % (unsigned)radix];
        value /= (unsigned)radix;
    } while (value);

    while (n) {
        *p++ = buf[--n];
    }
    *p = '\0';
    return s;
}
