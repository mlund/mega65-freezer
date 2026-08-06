#pragma once

/* MEGA65 only: writes the VIC-IV screen directly. */

void setup_screen(void);
void display_footer(unsigned char index);
void write_line(const char* s, char col);
void write_line_len(const char* s, char col, char length);
void write_line_raw(char* s, char col, char length);
void recolour_last_line(char colour);
void recolour_last_line_segment(unsigned char column, unsigned char width, unsigned char colour);
void screen_colour_line(unsigned char line, unsigned char colour);
#define screen_colour_line_segment(LA, W, C) lfill((LA) + (0x1f800 - SCREEN_ADDRESS), C, W)
void set_screen_attributes(long p, unsigned char count, unsigned char attr);
char read_line(char* buffer, unsigned char maxlen, unsigned char column);
void write_prompt(const char* s, char length);
void next_line(void);
void fatal_error(const unsigned char* filename, unsigned int line_number);
#define FATAL_ERROR fatal_error(__FILE__, __LINE__)
