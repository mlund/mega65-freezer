#ifndef __HELPER_H__
#define __HELPER_H__

#include "mega65.inc"

#include <stdint.h>

/* Implemented in helper.S.  leaf promises the callee does not re-enter C, so
 * callers keep their statically allocated frames instead of the soft stack.
 * It holds here: helper.s only ever transfers to LOADER_STUB and
 * PROGRAM_ENTRY, neither of which is C. */
#define HELPER_ASM __attribute__((leaf))

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
    unsigned char lo, hi;
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

#endif /* __HELPER_H__ */