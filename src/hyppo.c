/* What the tools ask of the hypervisor, over the SDK's trap wrappers.
 *
 * The wrappers state each trap's registers and clobbers; what belongs here is
 * the part above them -- which traps a job takes, in what order, and what the
 * answer means to us.  Traps the SDK does not wrap, and the exec path that has
 * to outlive the image it is loading over, stay in helper.S. */

#include "helper.h"

/* The flag byte dos_attach takes: bit 0 picks the drive, bit 1 asks for both
 * at once, bit 6 leaves the real floppy drive disconnected, and bit 7 makes a
 * detach of it (mega65-core, src/hyppo/dos.asm, dos_attach).  Detaching keeps
 * bits 0 and 6 only: the tools detach one named drive, so letting bit 1 pass
 * would turn that into both. */
static constexpr uint8_t DRIVE_MASK = 0x01;
static constexpr uint8_t DETACH_DRIVE_MASK = 0x41;
static constexpr uint8_t DETACH_FLAG = 0x80;
/* Pre-1.3 hyppo numbers the two drives as separate functions. */
static constexpr uint8_t ATTACH_LEGACY_BASE = 0x40;
static constexpr uint8_t ATTACH_LEGACY_DRIVE1 = 0x06;
/* dos_attach gained its current calling convention in hyppo DOS 1.3. */
static constexpr uint8_t HDOS_MAJOR_MIN = 1;
static constexpr uint8_t HDOS_MINOR_MIN = 3;
/* What a failed attach reports when hyppo has no code of its own to give. */
static constexpr uint8_t ATTACH_ERROR = 0xEF;

/* Starts true so that a tool which never calls mega65_dos_init() uses the
 * current attach convention rather than the pre-1.3 one. */
bool hdos_new_attach = true;

void mega65_dos_init(void) {
    const mega65_h_version hyppo = mega65_h_getversion();
    hdos_new_attach = hyppo.hdos_major > HDOS_MAJOR_MIN ||
        (hyppo.hdos_major == HDOS_MAJOR_MIN && hyppo.hdos_minor >= HDOS_MINOR_MIN);
}

/* Name the file the next trap acts on, leaving the pointer where the monitor
 * can find it afterwards.  Stashed before the call, not after: the failure is
 * what a post-mortem is for, and the pointer is dead once the trap has it, so
 * nothing has to survive the call. */
static mega65_h_err named(const char* name) {
    volatile uint8_t* const stash = hyppo_page + 0x40;
    const uintptr_t at = (uintptr_t)name;
    stash[0] = (uint8_t)at;
    stash[1] = (uint8_t)(at >> 8);
    return mega65_h_setname(name);
}

/* No stash: a load names a file the caller already has in hand, and the tools
 * that only ever load one pay for the stores otherwise. */
uint8_t read_file_from_sdcard(const char* filename, uint32_t load_address) {
    const mega65_h_err err = mega65_h_setname(filename);
    return err != MEGA65_H_OK ? err : mega65_h_loadfile(load_address);
}

uint8_t mega65_dos_chdir(const char* dirname) {
    mega65_h_err err = named(dirname);
    if (err == MEGA65_H_OK) {
        err = mega65_h_findfile();
    }
    return err == MEGA65_H_OK ? mega65_h_chdir() : err;
}

uint8_t mega65_dos_cdroot(void) {
    return mega65_h_cdrootdir(mega65_h_getcurrentdrive());
}

/* Pre-1.3 hyppo has no dos_attach: each drive is its own function number, and
 * the SDK wraps neither, both being deprecated in the core.
 *
 * Answers the raw error byte rather than mega65_h_err, because ATTACH_ERROR is
 * ours and not one of the codes that enum names. */
static uint8_t attach_legacy(uint8_t drive) {
    /* A carries the function number in and the error code out, so one
     * read-write operand rather than two: the trap answers where it was asked. */
    uint8_t fn = ATTACH_LEGACY_BASE + (drive ? ATTACH_LEGACY_DRIVE1 : 0);
    __asm__ volatile("sta %c[trap]\n\tclv\n\tbcc 1f\n\tlda #0\n1:"
        : "+a"(fn)
        : [trap] "i"(HTRAP_DOS)
        : "x", "y", "p", "memory");
    /* A failure with nothing in A would otherwise read as success. */
    return fn == MEGA65_H_OK ? ATTACH_ERROR : fn;
}

uint8_t mega65_dos_attach(const char* image_name, uint8_t driveid) {
    const mega65_h_err err = named(image_name);
    if (err != MEGA65_H_OK) {
        return err;
    }
    const uint8_t drive = driveid & DRIVE_MASK;
    return hdos_new_attach ? mega65_h_attach(drive) : attach_legacy(drive);
}

void mega65_dos_detach(uint8_t driveid_and_flags) {
    /* Detach is dos_attach with bit 7, so the same wrapper does both. */
    (void)mega65_h_attach((driveid_and_flags & DETACH_DRIVE_MASK) | DETACH_FLAG);
}
