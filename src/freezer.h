#pragma once

#include "errors.h"

#include <ctype.h>
#include <mega65/dirent.h>
#include <mega65/fileio.h>
#include <stdint.h>

#define WITH_AUDIOMIXER
// #define WITH_TOUCH

#include "helper.h"

void freeze_monitor(void);

#define SELDISK_INTERNAL (char*)0xFFFF
#define SELDISK_NODISK (char*)0xFFFE
#define NO_DISK_DRIVE "- NO DISK -"
#define INTERNAL_DRIVE_0 "- INTERNAL 3.5\" -"
#define INTERNAL_DRIVE_1 "- 1565 DRIVE 1 -"
char* freeze_select_disk_image(unsigned char drive_id);

void request_freeze_region_list(void);
uint32_t address_to_freeze_slot_offset(uint32_t address);
uint32_t find_thumbnail_offset(void);
unsigned char freeze_peek(uint32_t addr);
void freeze_poke(uint32_t addr, unsigned char v);
enum FreezerError freeze_fetch_sector(uint32_t addr, unsigned char* buffer);
enum FreezerError freeze_fetch_sector_partial(uint32_t addr, uint32_t dest, uint16_t count);
enum FreezerError freeze_store_sector(uint32_t addr, unsigned char* buffer);
enum FreezerError freeze_store_sector_partial(uint32_t addr, uint32_t src, uint16_t count);
void do_audio_mixer(void);
void do_sprite_editor(void);
unsigned char do_rom_loader(void);
void do_megainfo(void);

/* Only the lower 24 bits of region_length are valid. */
constexpr uint32_t REGION_LENGTH_MASK = 0x7FFFFF;

struct FreezeRegion {
    uint32_t address_base;
    union {
        uint32_t region_length; // only lower 24 bits are valid, space occupied rounded up to
                                // next 512 bytes
        struct {
            unsigned char skip[3];
            unsigned char freeze_prep;
        };
    };
};

constexpr uint8_t MAX_REGIONS = 256 / sizeof(struct FreezeRegion);

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
