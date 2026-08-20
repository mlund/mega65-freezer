#pragma once

// Hypervisor interface: the trap and loader constants, the pages hyppo reads
// and writes, and the routines hyppo.c and helper.S export between them.
//
// Included by helper.S as well as by C, so everything C-specific sits behind
// __ASSEMBLER__.  Do not parenthesise the constants: in 6502 assembly
// "(expr)" is indirect addressing, so "sta (0x0140)" means something else.

// Trap register.  The address chooses the handler, not the value: the CPU
// copies it into the program counter, so each of $D640-$D67F enters a
// different trap (mega65-core, src/vhdl/gs4510.vhdl, "set PC based on address
// written to").  A then chooses the function within that handler, and each
// handler numbers its own.
#define HTRAP_DOS 0xD640

// The one HTRAP_DOS function helper.S still takes for itself; everything else
// reaches hyppo through the SDK's wrappers, which name their own.
#define HYPPO_LOADFILE 0x36

// Hyppo's two page-aligned buffers are reserved in lowmem.ld, which is where
// the reasoning for their addresses lives: hyppo_page for the filename the
// file traps read and the descriptor and region list they copy back,
// hyppo_exec_page for an exec name that has to outlive that page.

// PRG conventions: files load at $07FF and are entered via the BASIC stub.
#define LOAD_ADDR_LO 0xFF
#define LOAD_ADDR_HI 0x07
#define PROGRAM_ENTRY 0x080D

#define NMI_VECTOR 0x0318
#define BORDER_COLOUR 0xD020

// Drive selection bits for attach/detach.
// The exec filename pointer, kept beside its buffer for post-mortem inspection
// from the monitor.  Unparenthesised for the reason at the top of this file,
// hence the NOLINT.
#define NAME_PTR_STASH_EXEC hyppo_exec_page + 0x40 // NOLINT(bugprone-macro-parentheses)

// Border flashes while retrying a failed load, before falling back.
#define RETRY_FLASH_COUNT 60

#ifndef __ASSEMBLER__

#include <mega65.h>
#include <stdbool.h>
#include <stdint.h>

/* Reserved in lowmem.ld.  Whatever hyppo last put here decides which shape is
 * meaningful, so the page is declared as bytes and each reader casts. */
extern volatile uint8_t hyppo_page[256];

/* The exec name's page, which must outlive hyppo_page's next use. */
extern volatile uint8_t hyppo_exec_page[256];

/* hyppo_page seen as what readdir leaves there, one of the shapes the page
 * takes in turn. */
#define HYPPO_DIRENT ((mega65_h_dirent*)(void*)hyppo_page)

/* The two directory entries every listing has to skip or notice.  Tested by
 * length rather than with strcmp, which lowers to memcmp and links 59 bytes
 * of it in for a one- and a two-character comparison. */
[[nodiscard]] static inline bool dirent_is_dot(const char* name, uint8_t len) {
    return len == 1 && name[0] == '.';
}

[[nodiscard]] static inline bool dirent_is_dotdot(const char* name, uint8_t len) {
    return len == 2 && name[0] == '.' && name[1] == '.';
}

/* The trap takes the page, not the address: the high byte is all it reads. */
#define HYPPO_PAGE_MSB ((uint8_t)((uintptr_t)hyppo_page >> 8))

/* Implemented in helper.S.  leaf promises the callee does not re-enter C, so
 * callers keep their statically allocated frames instead of the soft stack.
 * It holds here: helper.s only ever transfers to the loader stub and
 * PROGRAM_ENTRY, neither of which is C. */
#define HELPER_ASM __attribute__((leaf))

extern bool hdos_new_attach;

HELPER_ASM void init_nmi(void);

/* Record whether hyppo's DOS is new enough for the current dos_attach
 * convention, for mega65_dos_attach() to read. */
void mega65_dos_init(void);

/* Mount a D81 image on a drive.  0 on success, else hyppo's error code. */
uint8_t mega65_dos_attach(const char* image_name, uint8_t driveid);
#define M65_DOS_ATTACH_NODRIVE 0b01000000

/* Unmount whatever is on a drive.  Nothing to report: the drive ends up empty
 * whether or not anything was there. */
void mega65_dos_detach(uint8_t driveid_and_flags);

/* Change to a named directory, and to the current drive's root.  0 on
 * success, else hyppo's error code. */
uint8_t mega65_dos_chdir(const char* dirname);
uint8_t mega65_dos_cdroot(void);

/* Load another tool over this one and enter it.  Only returns on failure,
 * having fallen back to the freeze menu. */
HELPER_ASM char mega65_dos_exechelper(char* filename);

/* Load a file off the card at a 28-bit address.  0 on success, else hyppo's
 * error code. */
uint8_t read_file_from_sdcard(const char* filename, uint32_t load_address);

#endif // __ASSEMBLER__
