#pragma once

/* MEGA65 only: writes the VIC-IV screen directly. */

void setup_screen(void);
void display_footer(unsigned char index);
void write_line(const char* text, char column);
void write_line_len(const char* text, char column, char length);
void write_line_raw(char* text, char column, char length);
void recolour_last_line(char colour);
void recolour_last_line_segment(unsigned char column, unsigned char width, unsigned char colour);
void set_screen_attributes(long screen_address, unsigned char count, unsigned char attr);
char read_line(char* buffer, unsigned char max_length, unsigned char column);
void write_prompt(const char* text, char length);
void next_line(void);
void fatal_error(const unsigned char* filename, unsigned int line_number);
#define FATAL_ERROR fatal_error(__FILE__, __LINE__)
