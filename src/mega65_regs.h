#ifndef __MEGA65_REGS_H__
#define __MEGA65_REGS_H__

// MEGA65 hardware the SDK's <mega65.h> does not name, plus wrappers for the
// CPU's math accelerator.  C only -- helper.S has no use for any of it.

#include <mega65.h>
#include <stdint.h>

// The 45GS02's 32-bit multiplier and divider both run continuously off their
// inputs, so there is no start signal.  The multiplier settles in one cycle;
// the divider takes up to 20, so only division polls DIVBUSY.  The barrier
// keeps the input writes from being reordered past the result read.
#define MATH_DIVBUSY 0x80

static inline uint32_t hw_mul32(uint32_t a, uint32_t b) {
    MATH.multina32 = a;
    MATH.multinb32 = b;
    __asm__ volatile("" ::: "memory");
    return MATH.multout32;
}

static inline uint32_t hw_div32(uint32_t a, uint32_t b) {
    MATH.multina32 = a;
    MATH.multinb32 = b;
    __asm__ volatile("" ::: "memory");
    while (MATHBUSY & MATH_DIVBUSY)
        continue;
    return MATH.divout_whole32;
}

// Rounded up.  The divider yields the whole and fractional parts together, so
// this costs no second division.
static inline uint32_t hw_div32_ceil(uint32_t a, uint32_t b) {
    uint32_t whole = hw_div32(a, b);
    if (MATH.divout_fract32)
        whole++;
    return whole;
}

// 16-bit variants, for callers whose result cannot exceed 65535.  The inputs
// are still 32-bit -- that is what the hardware takes -- but rounding and
// returning in 16 bits saves the wider arithmetic the 32-bit forms need.
static inline uint16_t hw_mul16(uint16_t a, uint16_t b) {
    MATH.multina32 = a;
    MATH.multinb32 = b;
    __asm__ volatile("" ::: "memory");
    return MATH.multout16;
}

static inline uint16_t hw_div16(uint32_t a, uint32_t b) {
    MATH.multina32 = a;
    MATH.multinb32 = b;
    __asm__ volatile("" ::: "memory");
    while (MATHBUSY & MATH_DIVBUSY)
        continue;
    return MATH.divout_whole16;
}

static inline uint16_t hw_div16_ceil(uint32_t a, uint32_t b) {
    uint16_t whole = hw_div16(a, b);
    if (MATH.divout_fract32)
        whole++;
    return whole;
}

// Registers used only from C, so they are lvalues rather than bare addresses:
// ASCIIKEY reads and assigns directly instead of through PEEK/POKE.  Guarded
// because helper.S includes this file and cannot parse a cast.
// The VIC-III/IV conceals its registers until this knock is written to
// VICIV.key ($D02F), in this order.
#define VIC4_KNOCK_1 0x47
#define VIC4_KNOCK_2 0x53

#define MMIO8(addr) (*(volatile uint8_t*)(addr))

// F011 floppy controller (45IO27).  See the MEGA65 user guide, "45IO27
// Multi-Function I/O Controller".
#define F011_CONTROL MMIO8(0xD080) // motor and drive select
#define F011_COMMAND MMIO8(0xD081)
#define F011_STATUS MMIO8(0xD082) // BUSY is bit 7
#define F011_DATA MMIO8(0xD087)   // sector byte port
#define F011_CMD_SPINUP 0x20      // guide: write to COMMAND, then poll BUSY
#define F011_STATUS_BUSY 0x80     // guide: bit 7 of STATUS
// The rest are read off this program's own usage, not the user guide, which
// documents only SPINUP and BUSY.  Treat them as less certain.
#define F011_CMD_READ_SECTOR 0x41
#define F011_CMD_STEP 0x10       // steps the head one track
#define F011_CMD_SEEK 0x18       // seeks to the requested track
#define F011_CTRL_MOTOR_LED 0x60 // motor and drive LED on; OR in the drive id
#define F011_STATUS_AT_TRACK0 0x01
#define F011_STATUS_READ_ERROR 0x18 // sector read failed

// SD controller and touch panel.
#define SD_COMMAND MMIO8(0xD680)
#define SD_STATUS MMIO8(0xD680) // same register; reads status
#define SD_STATUS_SDHC 0x10
#define TOUCH_STATUS MMIO8(0xD6B0)
#define TOUCH_STATUS_EV1_VALID 0x01

// Keyboard event queue: reads the top event as ASCII, 0x00 when empty;
// assigning any value dequeues.  See the user guide, "Keyboard".
#define ASCIIKEY MMIO8(0xD610)

#endif /* __MEGA65_REGS_H__ */
