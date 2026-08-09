#pragma once

#include "errors.h"
#include "helper.h"
#include "slotmap.h"

#include <ctype.h>
#include <mega65/dirent.h>
#include <mega65/fileio.h>
#include <stddef.h>
#include <stdint.h>

void freeze_monitor(void);

/* ROMLOAD leaves this in the border for the freezer to read after the launch.
 * Outside the sixteen entries a scheme owns, so no role can collide with it. */
constexpr uint8_t BORDER_SIGNAL_ROM_CHANGED = 0x83;

#define SELDISK_INTERNAL (char*)0xFFFF
#define SELDISK_NODISK (char*)0xFFFE
#define NO_DISK_DRIVE "- NO DISK -"
#define INTERNAL_DRIVE_0 "- INTERNAL 3.5\" -"
#define INTERNAL_DRIVE_1 "- 1565 DRIVE 1 -"
/* The disk chooser leaves these for MAKEDISK, which mega65_dos_exechelper()
 * loads over it.  Placed by src/freezemenu.ld, which owns the low-memory map.
 * volatile because the reader is a different program. */
extern volatile uint8_t tool_density[1]; /* 0 = DD, 1 = HD */
extern volatile uint8_t tool_drive_id[1];

char* freeze_select_disk_image(unsigned char drive_id);

void request_freeze_region_list(void);
unsigned char freeze_peek(uint32_t addr);
void freeze_poke(uint32_t addr, unsigned char v);

/* The same, for the frozen program's saved I/O registers, named by their
 * register number: freeze_io_peek(0x3054) is its $FFD3054.  Still the freeze
 * slot, not live hardware -- see freeze_peek. */
unsigned char freeze_io_peek(uint16_t reg);
void freeze_io_poke(uint16_t reg, unsigned char v);
enum FreezerError freeze_fetch_sector(uint32_t addr, unsigned char* buffer);
enum FreezerError freeze_fetch_sector_partial(uint32_t addr, uint32_t dest, uint16_t count);
enum FreezerError freeze_store_sector(uint32_t addr, unsigned char* buffer);
enum FreezerError freeze_store_sector_partial(uint32_t addr, uint32_t src, uint16_t count);
void do_audio_mixer(void);
void do_sprite_editor(void);
unsigned char do_rom_loader(void);
void do_megainfo(void);

extern unsigned char not_in_root;
extern struct FreezeRegion freeze_region_list[MAX_REGIONS];
extern unsigned char freeze_region_count;

/* Bits of freeze_region_flags. */
enum : uint8_t {
    FreezeRegionHasChargen = 0x01,
};
extern unsigned char freeze_region_flags;

extern uint32_t freeze_slot_start_sector;
/* Runs the SYSPART_SLOT_SECTOR trap and returns the slot's start sector, which
 * the hypervisor leaves in $D681-$D684 rather than in a return value. */
uint32_t read_freeze_slot_start_sector(uint16_t slot);

constexpr uint8_t FD_DISK_ID_FILE_CLOSED = 0xFF;

struct FileDescriptor {
    unsigned char disk_id;
    uint32_t start_cluster;
    uint32_t current_cluster;
    unsigned char sector_in_cluster;
    uint32_t file_length;
    uint32_t buffer_position;
    uint32_t directory_cluster;
    uint16_t entry_in_directory;
    uint32_t buffer_address;
    uint16_t bytes_in_buffer;
    uint16_t offset_in_buffer;
};

/* Bits of ProcessDescriptor::d81_imageN_flags. */
enum : uint8_t {
    PdImgFlagsMounted = 0b00000001,
    PdImgFlagsWriteEn = 0b00000101,
    PdImgFlagsNoReal = 0b01000000,
};
struct ProcessDescriptor {
    unsigned char task_id;
    char process_name[16];
    unsigned char d81_image0_flags;
    unsigned char d81_image1_flags;
    unsigned char d81_image0_namelen;
    unsigned char d81_image1_namelen;
    char d81_image0_name[32];
    char d81_image1_name[32];
    char filler[0x80 - (1 + 16 + 1 + 1 + 1 + 1 + 32 + 32)];
    struct FileDescriptor file_descriptors[4];

    // Pad out to whole sector size, so we can load it easily
    char padding[256];
};

/* Hyppo writes this layout, so the offsets are its to choose, not ours: the
 * fields are grouped by kind rather than by drive.  Asserted because the two
 * per-drive members of each pair are adjacent by that choice alone, and code
 * that walks them as a pair would break silently if a field moved. */
static_assert(offsetof(struct ProcessDescriptor, d81_image0_flags) == 0x11);
static_assert(offsetof(struct ProcessDescriptor, d81_image1_flags) == 0x12);
static_assert(offsetof(struct ProcessDescriptor, d81_image0_namelen) == 0x13);
static_assert(offsetof(struct ProcessDescriptor, d81_image1_namelen) == 0x14);
static_assert(offsetof(struct ProcessDescriptor, d81_image0_name) == 0x15);
static_assert(offsetof(struct ProcessDescriptor, d81_image1_name) == 0x35);
