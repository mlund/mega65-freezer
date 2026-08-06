#pragma once

#include <stdint.h>

/* cc65 extensions the llvm-mos libc does not declare. */
char* itoa(int, char*, int);
char* utoa(unsigned, char*, int);

/* PETSCII key codes; mega65-libc's conio.h does not define them. */
constexpr uint8_t CH_CURS_UP = 145;
constexpr uint8_t CH_CURS_DOWN = 17;
constexpr uint8_t CH_CURS_LEFT = 157;
constexpr uint8_t CH_CURS_RIGHT = 29;
constexpr uint8_t CH_DEL = 20;
constexpr uint8_t CH_INS = 148;
constexpr uint8_t CH_ENTER = 13;
constexpr uint8_t CH_STOP = 3;
constexpr uint8_t CH_ESC = 27;
