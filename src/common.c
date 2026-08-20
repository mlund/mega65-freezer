/* What every tool needs before it can get to its own work: the palette and
 * colour scheme, which machine and ROM this is, and the pieces of frozen state
 * a tool reads or hands back. */

#include "common.h"

#include "colours.h"
#include "dma.h"
#include "mega65_regs.h"
#include "slot.h"

#include <mega65.h>
#include <string.h>

uint8_t sector_buffer[SD_SECTOR_SIZE];
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
    if (freeze_io_peek(0x367d) & 0x10) {
        return 40;
    }
    if (freeze_io_peek(0x3054) & 0x40) {
        return 40;
    }
    if (freeze_io_peek(0x3031) & 0x40) {
        return 3;
    }
    if (freeze_io_peek(0x0030) & 0x01) {
        return 2;
    }
    return 1;
}

void freezer_tool_start(void) {
    /* Performs the $D02F knock; without it every later write to a VIC-IV
     * register such as $D054 is silently ignored and the screen mode is never
     * established. */
    mega65_fast();

    /* The two CIAs and the VIC only: the C65 UART and the ethernet controller
     * can still raise an interrupt here. */
    __asm__ volatile("sei" ::: "memory");
    CIA1.icr = CIA_ICR_DISABLE_ALL;
    CIA2.icr = CIA_ICR_DISABLE_ALL;
    VICIV.imr = 0x00;

    // Bank out BASIC ROM, leave KERNAL and IO in
    CPU_PORTDDR = CPU_PORT_DDR_ALL_OUTPUTS;
    CPU_PORT = CPU_PORT_KERNAL_AND_IO;

    // No decimal mode!
    __asm__ volatile("cld");

    // Extended attributes, so a tool can draw in reverse
    VICIV.ctrlb = VICIV.ctrlb | VIC4_CTRLB_EXTENDED_ATTRIBUTES;
    VICIV.chrxscl = VIC4_CHRXSCL_80_COLUMN;

    SID1.amp = 0;
    SID2.amp = 0;

    set_palette();

    sdhc_card = (SD_STATUS & SD_STATUS_SDHC) != 0;
}

static char default_error[] = "ERROR CODE XX";
bool hyppo_file_absent(uint8_t error) {
    /* Not folded into hyppoerror_to_screen(): $FF is also "no such trap", so
     * the two are the same answer only where a file was being looked for. */
    return error == 0x88 || error == 0xFF;
}

char* hyppoerror_to_screen(uint8_t error) {
    // Few messages: each costs its full width in the image.
    switch (error) {
            /*
              case 0x07:
                return "READ TIMEOUT";
              case 0x11:
                return "ILLEGAL VALUE";
            */
        case 0x20:
            return "READ ERROR";
            /*
              case 0x21:
                return "WRITE ERROR";
              case 0x80:
                return "NO SUCH DRIVE";
              case 0x81:
                return "NAME TO LONG";
              case 0x82:
                return "NOT IMPLEMENTED";
              case 0x83:
                return "FILE TO LONG";
              case 0x84:
                return "TO MANY OPEN FILES";
            */
        case 0x85:
            return "INVALID CLUSTER";
            /*
              case 0x86:
                return "IS A DIRECTORY";
              case 0x87:
                return "NOT A DIRECTORY";
            */
        case 0x88:
            return "FILE NOT FOUND";
            /*
              case 0x89:
                return "INVALID FILE DESCR";
            */
        case 0x8a:
            return "WRONG IMAGE LENGTH";
        case 0x8b:
            return "IMAGE FRAGMENTED";
            /*
              case 0x8c:
                return "NO SPACE LEFT";
              case 0x8d:
                return "FILE EXISTS";
              case 0x8e:
                return "DIRECTORY FULL";
              case 0xff:
                return "NO SUCH TRAP / EOF";
            */
        default:
            break;
    }
    default_error[11] = (error >> 4) + (((error >> 4) < 10) ? 0x30 : 0x37);
    default_error[12] = (error & 0xf) + (((error & 0xf) < 10) ? 0x30 : 0x37);
    return default_error;
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

/* The frozen copy of the descriptor.  Its region starts on a sector boundary,
 * so a store needs no read-before-write and the sector goes back out as it came
 * in bar the patch: one read and one write, whatever the field. */
static constexpr uint32_t FROZEN_PROCDESC = 0xFFFBD00L;
static constexpr uint8_t PD_FLAGS = offsetof(struct ProcessDescriptor, d81_flags);
static constexpr uint8_t PD_NAMELEN = offsetof(struct ProcessDescriptor, d81_namelen);
static constexpr uint8_t PD_NAME = offsetof(struct ProcessDescriptor, d81_name);
static constexpr uint8_t PD_NAME_BYTES = 32;

/* The frozen CPU's saved state, at the base of its own region. */
static constexpr uint32_t FROZEN_CPU_STATE = 0xFFD3640UL;

void freeze_reset_cpu_state(void) {
    /* The reset vector is read first: it is in another sector, and freeze_peek
     * would reload sector_buffer over the patch below. */
    const unsigned char reset_lo = freeze_peek(0x2FFFCL);
    const unsigned char reset_hi = freeze_peek(0x2FFFDL);

    /* Offsets $07 to $11 are eleven contiguous bytes of one sector, so this is
     * one read and one write rather than eleven of each; the region begins on a
     * sector boundary, so the store needs no read-before-write. */
    if (freeze_fetch_sector(FROZEN_CPU_STATE, NULL) == FreezerOk) {
        sector_buffer[0x07] = 0xe7;     // disable interrupts, clear decimal mode
        sector_buffer[0x08] = reset_lo; // PC to the reset vector
        sector_buffer[0x09] = reset_hi;
        for (uint8_t i = 0x0a; i <= 0x0f; i++) {
            sector_buffer[i] = 0; // clear memory mapping
        }
        sector_buffer[0x10] = 0x3f; // reset $01 port values
        sector_buffer[0x11] = 0x3f;
        freeze_store_sector(FROZEN_CPU_STATE, NULL);
    }
    // Turn off extended graphics mode, only keep palemu
    freeze_poke(0xFFD3054U, freeze_peek(0xFFD3054U) & 0x20);
}

void copy_imageproc_to_freezeregion(uint8_t diskid, uint8_t overrides) {
    uint8_t i;

    (void)mega65_h_get_proc_desc(HYPPO_PAGE_MSB);

    i = HYPPO_PROCDESC->d81_flags[diskid];
    // write enable fix for HDOS < 1.3
    if (!hdos_new_attach && i == 1) {
        i = 5;
    }

    /* freeze_poke reads a whole 512-byte sector, changes one byte and writes it
     * back, so these thirty-four bytes cost sixty-eight transfers written
     * singly -- and this runs twice on every boot. */
    if (freeze_fetch_sector(FROZEN_PROCDESC, NULL) != FreezerOk) {
        return;
    }
    sector_buffer[PD_FLAGS + diskid] = overrides ? (overrides & ImgProcNoDisk ? 0x40 : 0) : i;
    sector_buffer[PD_NAMELEN + diskid] = overrides ? 0 : HYPPO_PROCDESC->d81_namelen[diskid];
    for (i = 0; i < PD_NAME_BYTES; i++) {
        sector_buffer[PD_NAME + diskid * PD_NAME_BYTES + i] =
            overrides ? 0 : (unsigned char)HYPPO_PROCDESC->d81_name[diskid][i];
    }
    freeze_store_sector(FROZEN_PROCDESC, NULL);
}

void old_store_selected_disk_image(uint8_t diskid, const char* disk_image) {
    uint8_t flags;
    uint8_t i;

    // reflect mount hyppo mount state into image flags
    if (lpeek(0xFFD368B) & (diskid ? 0x08 : 0x01)) {
        flags = 0b00000101;
    } else {
        if (lpeek(0xFFD36A1) & (diskid ? 0x04 : 0x01)) {
            flags = 0;
        } else {
            flags = 0x40;
        }
    }

    /* Same sector for all three fields, so one read and one write. */
    if (freeze_fetch_sector(FROZEN_PROCDESC, NULL) != FreezerOk) {
        return;
    }
    sector_buffer[PD_FLAGS + diskid] = flags;
    for (i = 0; (i < PD_NAME_BYTES) && disk_image[i]; i++) {
        sector_buffer[PD_NAME + diskid * PD_NAME_BYTES + i] = (unsigned char)disk_image[i];
    }
    sector_buffer[PD_NAMELEN + diskid] = i;
    // Pad with spaces as required by hypervisor
    for (; i < PD_NAME_BYTES; i++) {
        sector_buffer[PD_NAME + diskid * PD_NAME_BYTES + i] = ' ';
    }
    freeze_store_sector(FROZEN_PROCDESC, NULL);
}
