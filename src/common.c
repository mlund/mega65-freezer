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
/* 20, not 12: the OpenROM probe copies 16 bytes to name + 4 and then reads
 * [11] to [13], so the shorter buffer it would otherwise fit does not. */
char mega65_rom_name[20];

uint8_t current_scheme = SCHEME_BOOT;

void apply_scheme(uint8_t scheme) {
    if (scheme >= SchemeCount) {
        scheme = 0;
    }
    current_scheme = scheme;
    scheme_handoff[0] = SCHEME_HANDOFF_MAGIC;
    scheme_handoff[1] = scheme;

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
    VICIV.palsel = 0xFF;

    /* Entries 16-255 never change: entry N holds the colour N itself names when
     * read as three bits of red, three of green and two of blue.  That is the
     * format the freezer's thumbnail stores, so its pixels index themselves. */
    for (uint8_t c = 16; c; c++) {
        PALETTE.red[c] = (c >> 4) & 0xe;
        PALETTE.green[c] = (c >> 1) & 0xe;
        PALETTE.blue[c] = (c << 2) & 0xf;
    }

    /* The tool that launched us leaves its choice in low memory; the magic
     * tells a real choice from whatever the frozen program had there. */
    apply_scheme(scheme_handoff[0] == SCHEME_HANDOFF_MAGIC ? scheme_handoff[1] : SCHEME_BOOT);
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

    mega65_rom_type = Mega65RomUnknown;
    strcpy(mega65_rom_name, "UNKNOWN    ");
    return mega65_rom_name;
}

/* The tests are taken in order and the first that matches wins, which reads
 * 40MHz from either flag alone.  Strictly it is FORCE or (VFAST and (FAST or
 * 2MHZ)), so a machine with VFAST set but neither FAST nor 2MHz is reported
 * faster than it runs. */
unsigned char detect_cpu_speed(void) {
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
    /* $FF is pi, which sits at $5E rather than with the letters. */
    return 0x5e;
}

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
