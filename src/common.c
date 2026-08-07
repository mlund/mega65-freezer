#include "common.h"

#include "colours.h"
#include "dma.h"
#include "mega65_regs.h"
#include "slot.h"

#include <mega65.h>
#include <string.h>

uint8_t sector_buffer[512];
uint16_t slot_number = 0;
enum Mega65Rom mega65_rom_type = Mega65RomUnknown;
/* 20, not 12: the OpenROM probe below does memcpy(name + 4, ..., 16), which
 * needs offsets 4..19.  At 12 it wrote eight bytes past the end and then read
 * [12] and [13] to identify the ROM. */
char mega65_rom_name[20];

uint8_t current_scheme = SCHEME_BOOT;

void apply_scheme(uint8_t scheme) {
    if (scheme >= SchemeCount) {
        scheme = 0;
    }
    current_scheme = scheme;
    POKE(SCHEME_HANDOFF_MAGIC_ADDR, SCHEME_HANDOFF_MAGIC);
    POKE(SCHEME_HANDOFF_INDEX_ADDR, scheme);

    /* One address computed here, then a walk in order, so no index is
     * multiplied per entry. */
    const struct SchemeSlot* slot = SCHEMES[scheme].entry;
    for (uint8_t c = 0; c < 16; c++) {
        PALETTE.red[c] = slot[c].red;
        PALETTE.green[c] = slot[c].green;
        PALETTE.blue[c] = slot[c].blue;
    }
}

void set_palette(void) {
    // set palette selector
    VICIV.palsel = 0xFF;

    /* Entries 16-255 never change: entry N holds the colour N itself names when
     * read as three bits of red, three of green and two of blue.  That is the
     * format the freezer's thumbnail stores, so its pixels index themselves. */
    for (uint8_t c = 16; c; c++) {
        // 3 bits for red
        PALETTE.red[c] = (c >> 4) & 0xe;
        // 3 bits for green
        PALETTE.green[c] = (c >> 1) & 0xe;
        // 2 bits for blue
        PALETTE.blue[c] = (c << 2) & 0xf;
    }

    /* The tool that launched us leaves its choice in low memory; the magic
     * tells a real choice from whatever the frozen program had there. */
    apply_scheme(PEEK(SCHEME_HANDOFF_MAGIC_ADDR) == SCHEME_HANDOFF_MAGIC
            ? PEEK(SCHEME_HANDOFF_INDEX_ADDR)
            : SCHEME_BOOT);
}

char* detect_rom(void) {
    unsigned char sector[32];

    // fetch memory from current slot (so change this before calling the function!)
    freeze_fetch_sector_partial(0x20000L, (long)sector, 32);

    // Check for C65 ROM via version string
    memcpy(mega65_rom_name + 4, sector + 0x16, 7);
    if ((mega65_rom_name[4] == 'V') && (mega65_rom_name[5] == '9')) {
        if (mega65_rom_name[6] >= '2') {
            mega65_rom_name[0] = 'M';
            mega65_rom_type = Mega65RomM65;
        } else {
            mega65_rom_name[0] = 'C';
            mega65_rom_type = Mega65RomC65;
        }
        mega65_rom_name[1] = '6';
        mega65_rom_name[2] = '5';
        mega65_rom_name[3] = ' ';
        mega65_rom_name[11] = 0;
        return mega65_rom_name;
    }

    // OpenROM - 16 characters "OYYMMDDCC       "
    memcpy(mega65_rom_name + 4, sector + 0x10, 16);
    if ((mega65_rom_name[4] == 'O') && (mega65_rom_name[11] == '2') &&
        (mega65_rom_name[12] == '0') && (mega65_rom_name[13] == ' ')) {
        mega65_rom_type = Mega65RomOpenRom;
        mega65_rom_name[0] = 'O';
        mega65_rom_name[1] = 'P';
        mega65_rom_name[2] = 'E';
        mega65_rom_name[3] = 'N';
        mega65_rom_name[4] = ' ';
        mega65_rom_name[11] = 0;
        return mega65_rom_name;
    }

#define COPY_AND_RETURN_ROM(X)                                                                     \
    {                                                                                              \
        strcpy(mega65_rom_name, X);                                                                \
        return mega65_rom_name;                                                                    \
    }

    /*
      The C64 ROM part can't really work without a real C64 cpu,
      so it is save to return UNKNOWN for now

      // entering C64 region
      mega65_rom_type = Mega65RomC64;

      if (freeze_peek(0x2e47dL) == 'J') {
        // Probably jiffy dos
        if (freeze_peek(0x2e535L) == 0x06)
          COPY_AND_RETURN_ROM("SX64 JIFFY ")
        else
          COPY_AND_RETURN_ROM("C64 JIFFY  ")
      }

      // Else guess using detection routines from detect_roms.c
      // These were built using a combination of the ROMs from zimmers.net/pub/c64/firmware,
      // the RetroReplay ROM collection, and the JiffyDOS ROMs
      if (freeze_peek(0x2e449L) == 0x2e)
        COPY_AND_RETURN_ROM("C64GS      ")
      if (freeze_peek(0x2e119L) == 0xc9)
        COPY_AND_RETURN_ROM("C64 REV1   ")
      if (freeze_peek(0x2e67dL) == 0xb0)
        COPY_AND_RETURN_ROM("C64 REV2 JP")
      if (freeze_peek(0x2ebaeL) == 0x5b)
        COPY_AND_RETURN_ROM("C64 REV3 DK")
      if (freeze_peek(0x2e0efL) == 0x28)
        COPY_AND_RETURN_ROM("C64 SCAND  ")
      if (freeze_peek(0x2ebf3L) == 0x40)
        COPY_AND_RETURN_ROM("C64 SWEDEN ")
      if (freeze_peek(0x2e461L) == 0x20)
        COPY_AND_RETURN_ROM("CYCLONE 1.0")
      if (freeze_peek(0x2e4a4L) == 0x41)
        COPY_AND_RETURN_ROM("DOLPHIN 1.0")
      if (freeze_peek(0x2e47fL) == 0x52)
        COPY_AND_RETURN_ROM("DOLPHIN 2AU")
      if (freeze_peek(0x2eed7L) == 0x2c)
        COPY_AND_RETURN_ROM("DOLPHIN 2P1")
      if (freeze_peek(0x2e7d2L) == 0x6b)
        COPY_AND_RETURN_ROM("DOLPHIN 2P2")
      if (freeze_peek(0x2e4a6L) == 0x32)
        COPY_AND_RETURN_ROM("DOLPHIN 2P3")
      if (freeze_peek(0x2e0f9L) == 0xaa)
        COPY_AND_RETURN_ROM("DOLPHIN 3.0")
      if (freeze_peek(0x2e462L) == 0x45)
        COPY_AND_RETURN_ROM("DOSROM V1.2")
      if (freeze_peek(0x2e472L) == 0x20)
        COPY_AND_RETURN_ROM("MERCRY3 PAL")
      if (freeze_peek(0x2e16dL) == 0x84)
        COPY_AND_RETURN_ROM("MERCRY NTSC")
      if (freeze_peek(0x2e42dL) == 0x4c)
        COPY_AND_RETURN_ROM("PET 4064   ")
      if (freeze_peek(0x2e1d9L) == 0xa6)
        COPY_AND_RETURN_ROM("SX64 CROACH")
      if (freeze_peek(0x2eba9L) == 0x2d)
        COPY_AND_RETURN_ROM("SX64 SCAND ")
      if (freeze_peek(0x2e476L) == 0x2a)
        COPY_AND_RETURN_ROM("TRBOACS 2.6")
      if (freeze_peek(0x2e535L) == 0x07)
        COPY_AND_RETURN_ROM("TRBOACS 3P1")
      if (freeze_peek(0x2e176L) == 0x8d)
        COPY_AND_RETURN_ROM("TRBOASC 3P2")
      if (freeze_peek(0x2e42aL) == 0x72)
        COPY_AND_RETURN_ROM("TRBOPROC US")
      if (freeze_peek(0x2e4acL) == 0x81)
        COPY_AND_RETURN_ROM("C64C 251913")
      if (freeze_peek(0x2e479L) == 0x2a)
        COPY_AND_RETURN_ROM("C64 REV2   ")
      if (freeze_peek(0x2e535L) == 0x06)
        COPY_AND_RETURN_ROM("SX64 REV4  ")
    */

    // set some flags
    mega65_rom_type = Mega65RomUnknown;
    COPY_AND_RETURN_ROM("UNKNOWN    ")
}

unsigned char detect_cpu_speed(void) {
    // Technically this is more correct, involving the 2 MHz flag:
    //   FORCE || (VFAST && (FAST || 2MHZ)) -> 40 MHz
    //
    // if ((freeze_peek(0xffd367dL) & 0x10) ||
    //     (freeze_peek(0xffd3054L) & 0x40) &&
    //         ((freeze_peek(0xffd3031L) & 0x40) ||
    //          (freeze_peek(0xffd0030L) & 0x01)))
    //    return 40;

    if (freeze_peek(0xffd367dL) & 0x10) {
        return 40;
    }
    if (freeze_peek(0xffd3054L) & 0x40) {
        return 40;
    }
    if (freeze_peek(0xffd3031L) & 0x40) {
        return 3;
    }
    if (freeze_peek(0xffd0030L) & 0x01) {
        return 2;
    }
    return 1;
}

/*
 * uint8_t nybl_to_screen(uint8_t v)
 *
 * converts the lower 4 bits of a byte to a screen code
 * hexadecimal number digit.
 */
uint8_t nybl_to_screen(uint8_t v) {
    v &= 0xf;
    if (v < 0xa) {
        return 0x30 + v;
    }
    return v - 0x9;
}

unsigned char petscii_to_screen(unsigned char petscii) {
    // control characters => space
    if ((petscii & 0x7f) < 0x20) {
        return 0x20;
    }
    if (petscii < 0x40) {
        return petscii;
    }
    if (petscii < 0x60) {
        return petscii & 0x3f;
    }
    if (petscii < 0x80) {
        return petscii & 0x5f;
    }
    if (petscii < 0xc0) {
        return petscii ^ 0xc0;
    }
    if (petscii < 0xe0) {
        return petscii & 0x5f;
    }
    if (petscii < 0xff) {
        return petscii & 0x7f;
    }
    // want some pi?
    return 0x5e;
}

// static char* deadly_haiku[3] = { "Error consumes all", "As sand erodes rock and stone", "Now also
// your mind" };

/* Nothing is trustworthy enough to draw with here, so the border is the whole
 * report.  The asm keeps the spin: a loop with no side effect is undefined and
 * the optimiser may delete it. */
void screen_of_death(const char* msg) {
    (void)msg;
    VICIV.bordercol = SchemeError;
    for (;;) {
        __asm__ volatile("");
    }
}

void copy_imageproc_to_freezeregion(uint8_t diskid, uint8_t overrides) {
    uint8_t disk_img_name_loc = diskid ? 0x35 : 0x15;
    uint8_t disk_img_flag_loc = diskid ? 0x12 : 0x11;
    uint8_t i;

    mega65_dos_getprocdesc(0x04); // get procdesc from hyppo to 0x400

    i = PEEK(0x0400U + disk_img_flag_loc);
    // write enable fix for HDOS < 1.3
    if (!hdos_new_attach && i == 1) {
        i = 5;
    }

    freeze_poke(
        0xFFFBD00L + disk_img_flag_loc, overrides ? (overrides & ImgProcNoDisk ? 0x40 : 0) : i);
    freeze_poke(0xFFFBD02L + disk_img_flag_loc,
        overrides ? 0 : PEEK(0x0402U + disk_img_flag_loc)); // this is namelength
    for (i = 0; i < 32; i++) {
        freeze_poke(0xFFFBD00L + disk_img_name_loc + i,
            overrides ? 0 : PEEK(0x0400U + disk_img_name_loc + i));
    }
}

void old_store_selected_disk_image(uint8_t diskid, char* disk_image) {
    uint8_t disk_img_name_loc = diskid ? 0x35 : 0x15;
    uint8_t disk_img_flag_loc = diskid ? 0x12 : 0x11;
    uint8_t i;

    // reflect mount hyppo mount state into image flags
    if (lpeek(0xFFD368B) & (diskid ? 0x08 : 0x01)) {
        i = 0b00000101;
    } else {
        if (lpeek(0xFFD36A1) & (diskid ? 0x04 : 0x01)) {
            i = 0;
        } else {
            i = 0x40;
        }
    }
    freeze_poke(0xFFFBD00L + disk_img_flag_loc, i);

    // Replace disk image name in process descriptor block
    for (i = 0; (i < 32) && disk_image[i]; i++) {
        freeze_poke(0xFFFBD00L + disk_img_name_loc + i, disk_image[i]);
    }
    // Update length of name
    freeze_poke(0xFFFBD02L + disk_img_flag_loc, i);
    // Pad with spaces as required by hypervisor
    for (; i < 32; i++) {
        freeze_poke(0xFFFBD00L + disk_img_name_loc + i, ' ');
    }
}
