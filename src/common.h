#pragma once

#include "mega65_regs.h" // ASCIIKEY and KEY_* below
#include "sdcard.h"

#include <stdint.h>

/* Abandons a long redraw when a cursor key is waiting, so holding one down
 * walks the freeze slots at the speed of the key rather than of the drawing.
 *
 * Clearing bit 7 folds each cursor key onto its opposite -- up reads as down,
 * left as right -- so two comparisons cover all four. */
#define NAVIGATION_KEY_CHECK()                                                                     \
    {                                                                                              \
        if (((ASCIIKEY & 0x7f) == KEY_CURSOR_DOWN) || ((ASCIIKEY & 0x7f) == KEY_CURSOR_RIGHT))     \
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

/* What every tool the freeze menu launches does before it draws anything: the
 * I/O knock, interrupts off, BASIC banked out, decimal mode off, the SIDs
 * silenced, the palette loaded and the card's kind established.
 *
 * FREEZER is not one of them and does this itself -- it installs an NMI
 * handler rather than a bare SEI, silences four SIDs, restores $DD00's data
 * direction and disables the cartridge, none of which a tool loaded over it
 * has to repeat. */
void freezer_tool_start(void);

char* detect_rom(void);
unsigned char detect_cpu_speed(void);
unsigned char petscii_to_screen(unsigned char petscii);
/* What a hyppo error code means, in ASCII, or "ERROR CODE XX" with the code in
 * hex when it is one of the many not worth its own string.  Shared so that two
 * tools reporting the same failure do not grow two lists of hyppo's codes. */
char* hyppoerror_to_screen(uint8_t error);

/* Whether a hyppo error says the file is not there.
 *
 * Two codes mean it, and which one comes back depends on whose hyppo: the
 * machine answers with the end of the directory, since dos_findfile() walks
 * entries until one matches and running out of them is EOF (mega65-core
 * src/hyppo/dos.asm:2450 and :2498), while Xemu's answers file-not-found.  A
 * screen that knows only one of them shows a bare code for the commonest
 * failure there is. */
[[nodiscard]] bool hyppo_file_absent(uint8_t error);
void screen_of_death(const char* msg);

/* Overrides for copy_imageproc_to_freezeregion(), OR-ed together. */
enum : uint8_t {
    ImgProcInternal = 0x01,
    ImgProcNoDisk = 0x02,
};
/* Point the frozen CPU at the reset vector and give it a C64 memory map, so a
 * resume starts the machine rather than the program it froze. */
void freeze_reset_cpu_state(void);
void copy_imageproc_to_freezeregion(uint8_t diskid, uint8_t overrides);
void old_store_selected_disk_image(uint8_t diskid, const char* disk_image);
