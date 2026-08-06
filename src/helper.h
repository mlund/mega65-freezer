#pragma once

// Hypervisor interface: the trap and loader constants helper.S writes, the
// routines it exports, and the short traps that are inline asm instead.
//
// Included by helper.S as well as by C, so everything C-specific sits behind
// __ASSEMBLER__.  Do not parenthesise the constants: in 6502 assembly
// "(expr)" is indirect addressing, so "sta (0x0140)" means something else.

// Trap registers.  The value written selects the function; the two registers
// reach different trap handlers.
#define HTRAP_DOS 0xD640

// HTRAP_DOS functions.
#define HYPPO_GETVERSION 0x00
#define HYPPO_GETCURRENTDRIVE 0x04
#define HYPPO_CHDIR 0x0C
#define HYPPO_CLOSEALL 0x22
#define HYPPO_SETNAME 0x2E
#define HYPPO_FINDFILE 0x34
#define HYPPO_LOADFILE 0x36
#define HYPPO_CDROOTDIR 0x3C
#define HYPPO_GETPROCDESC 0x48
#define HYPPO_ATTACH 0x4A

// HTRAP_SYSPART functions.

// Hyppo needs its filename buffer page-aligned and in the bottom 32KB.  Two
// are used so a pending attach name is not clobbered by an exec.
#define NAME_BUF_EXEC 0x0100
#define NAME_BUF_DOS 0x0400

// Scratch page for the loader stub, which must survive the load it performs.
#define LOADER_STUB 0x0340
#define LOADER_STUB_SIZE 0x80

// PRG conventions: files load at $07FF and are entered via the BASIC stub.
#define LOAD_ADDR_LO 0xFF
#define LOAD_ADDR_HI 0x07
#define PROGRAM_ENTRY 0x080D

#define NMI_VECTOR 0x0318
#define BORDER_COLOUR 0xD020

// Drive selection bits for attach/detach.
#define DRIVE_MASK 0x01
#define DETACH_DRIVE_MASK 0x41 // drive bits plus bit 6 (nodrive)
#define DETACH_FLAG 0x80
#define ATTACH_LEGACY_BASE 0x40 // pre-1.2 hyppo: $40 or $46
#define ATTACH_LEGACY_DRIVE1 0x06

// Offsets into hyppo's dirent, written to NAME_BUF_DOS.
#define HDIRENT_NAME_LEN 64 // name is 64 bytes
#define HDIRENT_CLUSTER 77  // 64 + 1 + 12
#define HDIRENT_SIZE 81     // HDIRENT_CLUSTER + 4
#define HDIRENT_ATTR 86     // HDIRENT_SIZE + 1 + 4

// Offsets into the C struct m65_dirent.
#define DIRENT_INO 0    // d_ino,    4 bytes
#define DIRENT_OFF 4    // d_off,    2 bytes
#define DIRENT_RECLEN 6 // d_reclen, 4 bytes
#define DIRENT_TYPE 10  // d_type,   2 bytes
#define DIRENT_NAME 12  // d_name

// The filename pointer is stashed alongside each buffer for post-mortem
// inspection from the monitor.
//
// Deliberately unparenthesised, despite bugprone-macro-parentheses: helper.S
// includes this header, and in 6502 assembly parentheses are the indirect
// addressing mode rather than grouping.  Wrapping the replacement list turns
// `sta NAME_PTR_STASH_EXEC` into an indirect store, which the assembler
// rejects outright ("operand must be an 8-bit address").
#define NAME_PTR_STASH_EXEC NAME_BUF_EXEC + 0x40 // NOLINT(bugprone-macro-parentheses)
#define NAME_PTR_STASH_DOS NAME_BUF_DOS + 0x40   // NOLINT(bugprone-macro-parentheses)

// dos_attach gained its current calling convention in hyppo DOS 1.3.
#define HDOS_MAJOR_MIN 1
#define HDOS_MINOR_MIN 3

// Border flashes while retrying a failed load, before falling back.
#define RETRY_FLASH_COUNT 60

#define ATTACH_ERROR 0xEF

#ifndef __ASSEMBLER__

#include <stdint.h>

/* Implemented in helper.S.  leaf promises the callee does not re-enter C, so
 * callers keep their statically allocated frames instead of the soft stack.
 * It holds here: helper.s only ever transfers to LOADER_STUB and
 * PROGRAM_ENTRY, neither of which is C. */
#define HELPER_ASM __attribute__((leaf))

/* C-only: helper.S never names these, so they need not be preprocessor
 * constants and the compiler can check their type. */
#ifndef __ASSEMBLER__
constexpr uint16_t HTRAP_SYSPART = 0xD642;
constexpr uint8_t HYPPO_OPENDIR = 0x12;
constexpr uint8_t HYPPO_READDIR = 0x14;
constexpr uint8_t HYPPO_CLOSEDIR = 0x16;
constexpr uint8_t SYSPART_SLOT_SECTOR = 0x10;
constexpr uint8_t SYSPART_UNFREEZE = 0x12;
constexpr uint8_t SYSPART_REGION_LIST = 0x14;
constexpr uint8_t SYSPART_SLOT_COUNT = 0x16;
constexpr uint8_t SYSPART_GETERRORCODE = 0x38;
constexpr uint8_t HDIRENT_NAME = 0;
constexpr uint16_t DIRENT_NAME_MAX = 256;
#endif

/* Traps short enough to state their own clobbers live here as inline asm, so
 * the compiler places operands in the registers the trap wants instead of the
 * assembly shuffling them out of the C ABI by hand.  Optimising for size, one
 * out-of-line copy beats duplicating the body per call site; otherwise inline. */
#if defined(__OPTIMIZE_SIZE__)
#define HELPER_INLINE static inline __attribute__((noinline))
#else
#define HELPER_INLINE static inline __attribute__((always_inline))
#endif

/* Slot number: low byte in Y, high in X. */
HELPER_INLINE void find_freeze_slot_start_sector(unsigned short slot) {
    __asm__ volatile("lda #%c[fn]\n\tsta %c[trap]\n\tclv"
        :
        : "y"((unsigned char)slot),
        "x"((unsigned char)(slot >> 8)),
        [fn] "i"(SYSPART_SLOT_SECTOR),
        [trap] "i"(HTRAP_SYSPART)
        : "a", "p", "memory");
}

/* Slot number: low byte in Y, high in X. */
HELPER_INLINE void unfreeze_slot(unsigned short slot) {
    __asm__ volatile("lda #%c[fn]\n\tsta %c[trap]\n\tclv"
        :
        : "y"((unsigned char)slot),
        "x"((unsigned char)(slot >> 8)),
        [fn] "i"(SYSPART_UNFREEZE),
        [trap] "i"(HTRAP_SYSPART)
        : "a", "p", "memory");
}

/* Buffer address: low byte in X, high in Y -- the opposite of the two above. */
HELPER_INLINE void fetch_freeze_region_list_from_hypervisor(unsigned short addr) {
    __asm__ volatile("lda #%c[fn]\n\tsta %c[trap]\n\tclv"
        :
        : "x"((unsigned char)addr),
        "y"((unsigned char)(addr >> 8)),
        [fn] "i"(SYSPART_REGION_LIST),
        [trap] "i"(HTRAP_SYSPART)
        : "a", "p", "memory");
}

/* Count returns in X(low)/Y(high). */
HELPER_INLINE unsigned short get_freeze_slot_count(void) {
    unsigned char lo;
    unsigned char hi;
    __asm__ volatile("lda #%c[fn]\n\tsta %c[trap]\n\tclv"
        : "=x"(lo), "=y"(hi)
        : [fn] "i"(SYSPART_SLOT_COUNT), [trap] "i"(HTRAP_SYSPART)
        : "a", "p", "memory");
    return lo | ((unsigned short)hi << 8);
}

HELPER_INLINE unsigned char mega65_geterrorcode(void) {
    unsigned char err;
    __asm__ volatile("lda #%c[fn]\n\tsta %c[trap]\n\tclv"
        : "=a"(err)
        : [fn] "i"(SYSPART_GETERRORCODE), [trap] "i"(HTRAP_SYSPART)
        : "p", "memory");
    return err;
}

extern uint8_t hdos_new_attach;

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

HELPER_ASM char read_file_from_sdcard(char* filename, uint32_t load_address);

#endif // __ASSEMBLER__
