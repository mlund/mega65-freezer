#pragma once

#include "sdcard.h"

#include <stdint.h>

// Used to quickly return from functions if a navigation key has been pressed
// (used to avoid delays when navigating through the list of freeze slots
#define NAVIGATION_KEY_CHECK()                                                                     \
    {                                                                                              \
        if (((ASCIIKEY & 0x7f) == 0x11) || ((ASCIIKEY & 0x7f) == 0x1D))                            \
            return;                                                                                \
    }

extern uint16_t slot_number;

constexpr Addr28 CHARGEN_ADDRESS = 0xFF7E000;

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

/* Fills all 256 palette entries and applies the scheme the freezer last chose.
 * Every tool calls this once at startup. */
void set_palette(void);
/* Rewrites the sixteen entries a scheme owns, and records the choice where the
 * next tool will find it.  Everything on screen changes on the next frame,
 * because colour RAM holds entry numbers rather than colours. */
void apply_scheme(uint8_t scheme);
/* The scheme currently loaded; index into SCHEMES[]. */
extern uint8_t current_scheme;

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
