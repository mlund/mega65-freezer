#pragma once

#include "fdisk_hal.h"

#include <stdint.h>

// Used to quickly return from functions if a navigation key has been pressed
// (used to avoid delays when navigating through the list of freeze slots
#define NAVIGATION_KEY_CHECK()                                                                     \
    {                                                                                              \
        if (((PEEK(0xD610U) & 0x7f) == 0x11) || ((PEEK(0xD610U) & 0x7f) == 0x1D))                  \
            return;                                                                                \
    }

extern unsigned short slot_number;

#define CHARGEN_ADDRESS 0xFF7E000L

/* Which ROM the frozen program is running. */
enum Mega65Rom : uint8_t {
    Mega65RomUnknown = 0,
    Mega65RomC64 = 1,
    Mega65RomC65 = 2,
    Mega65RomM65 = 3,
    Mega65RomOpenRom = 4,
};
extern enum Mega65Rom mega65_rom_type; // set by detect_rom()
extern char mega65_rom_name[];

void set_palette(void);
char* detect_rom(void);
unsigned char detect_cpu_speed(void);
uint8_t nybl_to_screen(uint8_t v);
unsigned char petscii_to_screen(unsigned char petscii);
void screen_of_death(const char* msg);

/* Overrides for copy_imageproc_to_freezeregion(), OR-ed together. */
enum : uint8_t {
    ImgProcInternal = 0x01,
    ImgProcNoDisk = 0x02,
};
void copy_imageproc_to_freezeregion(uint8_t diskid, uint8_t overrides);
void old_store_selected_disk_image(uint8_t diskid, char* disk_image);
