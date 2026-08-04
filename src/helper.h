#ifndef __HELPER_H__
#define __HELPER_H__

#include <stdint.h>

/* Implemented in helper.s.  leaf promises the callee does not re-enter C:
 * an opaque jsr might otherwise call anything, so the compiler must assume
 * every caller can be re-entered and move it off a statically allocated
 * frame onto the soft stack.  These are hypervisor traps and self-contained
 * routines -- the only transfers out of helper.s are to LOADER_STUB and
 * PROGRAM_ENTRY, neither of which is C in this program. */
#define HELPER_ASM __attribute__((leaf))

extern uint8_t hdos_new_attach;

HELPER_ASM unsigned char mega65_geterrorcode(void);

HELPER_ASM void init_nmi(void);

// initializes variables for attach call
// returns 0 if new dos_attach is not available, 1 otherwise
HELPER_ASM uint8_t mega65_dos_init(void);

HELPER_ASM char mega65_dos_attach(char* image_name, uint8_t driveid);
#define M65_DOS_ATTACH_NODRIVE 0b01000000
HELPER_ASM char mega65_dos_detach(uint8_t driveid_and_flags);

HELPER_ASM char mega65_dos_chdir(unsigned char* dirname);
HELPER_ASM char mega65_dos_cdroot();

HELPER_ASM char mega65_dos_exechelper(char* filename);

HELPER_ASM uint8_t mega65_dos_getprocdesc(uint8_t pagemsb);

HELPER_ASM unsigned short get_freeze_slot_count(void);
HELPER_ASM void fetch_freeze_region_list_from_hypervisor(unsigned short);
HELPER_ASM unsigned char find_freeze_slot_start_sector(unsigned short);
HELPER_ASM void unfreeze_slot(unsigned short);

HELPER_ASM char read_file_from_sdcard(char* filename, uint32_t load_address);

#endif /* __HELPER_H__ */