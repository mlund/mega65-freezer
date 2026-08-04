#ifndef __ASCII_H__
#define __ASCII_H__

/* The generated ascii.h is 256 identity #pragma charmap entries, which exist
 * to switch off cc65's default PETSCII remapping of character literals.
 * llvm-mos performs no such remapping, so nothing is needed here -- but the
 * shared .c files include this header for the cc65 build's sake, so it must
 * exist. */

#endif /* __ASCII_H__ */
