#pragma once

// MEGA65 hardware the SDK's <mega65.h> does not name, plus wrappers for the
// CPU's math accelerator.  C only -- helper.S has no use for any of it.

#include <mega65.h>
#include <stdint.h>

// The 45GS02's 32-bit multiplier and divider both run continuously off their
// inputs, so there is no start signal.  The multiplier settles in one cycle;
// the divider takes up to 20, so only division polls DIVBUSY.  The barrier
// keeps the input writes from being reordered past the result read.
constexpr uint8_t MATH_DIVBUSY = 0x80;

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
    while (MATHBUSY & MATH_DIVBUSY) {
    }
    return MATH.divout_whole32;
}

// Rounded up.  The divider yields the whole and fractional parts together, so
// this costs no second division.
static inline uint32_t hw_div32_ceil(uint32_t a, uint32_t b) {
    uint32_t whole = hw_div32(a, b);
    if (MATH.divout_fract32) {
        whole++;
    }
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
    while (MATHBUSY & MATH_DIVBUSY) {
    }
    return MATH.divout_whole16;
}

static inline uint16_t hw_div16_ceil(uint32_t a, uint32_t b) {
    uint16_t whole = hw_div16(a, b);
    if (MATH.divout_fract32) {
        whole++;
    }
    return whole;
}

// Registers used only from C, so they are lvalues rather than bare addresses:
// ASCIIKEY reads and assigns directly instead of through PEEK/POKE.  These stay
// macros because they must be assignable; the plain values above are constexpr
// so the compiler checks their type.
// The VIC-III/IV conceals its registers until this knock is written to
// VICIV.key ($D02F), in this order.
constexpr uint8_t VIC4_KNOCK_1 = 0x47;
constexpr uint8_t VIC4_KNOCK_2 = 0x53;

#define MMIO8(addr) (*(volatile uint8_t*)(addr))

// F011 floppy controller (45IO27).  See the MEGA65 user guide, "45IO27
// Multi-Function I/O Controller".
#define F011_CONTROL MMIO8(0xD080) // motor and drive select
#define F011_COMMAND MMIO8(0xD081)
#define F011_STATUS MMIO8(0xD082)          // BUSY is bit 7
#define F011_TRACK MMIO8(0xD084)           // FDC:TRACK
#define F011_SECTOR MMIO8(0xD085)          // FDC:SECTOR
#define F011_SIDE MMIO8(0xD086)            // FDC:SIDE
#define F011_DATA MMIO8(0xD087)            // sector byte port
constexpr uint8_t F011_CMD_SPINUP = 0x20;  // guide: write to COMMAND, then poll BUSY
constexpr uint8_t F011_STATUS_BUSY = 0x80; // guide: bit 7 of STATUS
// The rest are read off this program's own usage, not the user guide, which
// documents only SPINUP and BUSY.  Treat them as less certain.
constexpr uint8_t F011_CMD_READ_SECTOR = 0x41;
constexpr uint8_t F011_CMD_STEP = 0x10;       // steps the head one track
constexpr uint8_t F011_CMD_SEEK = 0x18;       // seeks to the requested track
constexpr uint8_t F011_CTRL_MOTOR_LED = 0x60; // motor and drive LED on; OR in the drive id
constexpr uint8_t F011_STATUS_AT_TRACK0 = 0x01;
constexpr uint8_t F011_STATUS_READ_ERROR = 0x18; // sector read failed

// SD controller and touch panel.
#define SD_COMMAND MMIO8(0xD680)
#define SD_STATUS MMIO8(0xD680) // same register; reads status
constexpr uint8_t SD_STATUS_SDHC = 0x10;
#define TOUCH_STATUS MMIO8(0xD6B0)
constexpr uint8_t TOUCH_STATUS_EV1_VALID = 0x01;
// TOUCH:TOUCH1XLSB / TOUCH1YLSB, with both MSB pairs sharing $D6BB.
#define TOUCH1_X_LSB MMIO8(0xD6B9)
#define TOUCH1_Y_LSB MMIO8(0xD6BA)
#define TOUCH1_MSB MMIO8(0xD6BB)
constexpr uint8_t TOUCH1_X_MSB_MASK = 0x03; // bits 0-1
constexpr uint8_t TOUCH1_Y_MSB_MASK = 0x30; // bits 4-5

// $D689 carries several unrelated signals; only the buffer select is used
// here.  SD:BUFSEL, 1 = the SD card's sector buffer, 0 = the F011/FDC's.
#define SD_MISC MMIO8(0xD689)
constexpr uint8_t SD_MISC_BUFSEL_SDCARD = 0x80;

// F011 disk-image type and control.  Bit 6 is drive 0 and bit 7 drive 1 in
// both, so the code shifts by the drive id.
#define SDFDC_IMAGE_TYPE MMIO8(0xD68A) // SDFDC:D0D64 / D1D64, set for D64
#define SDFDC_CONTROL MMIO8(0xD68B)    // SDFDC:D0MD / D1MD, set for D65

// AUDIOMIX:REGSEL selects a mixer coefficient, which is then read or written
// through AUDIOMIX:REGWDATA.
#define AUDIOMIX_REGSEL MMIO8(0xD6F4)
#define AUDIOMIX_REGDATA MMIO8(0xD6F5)

// VIC-IV video setup.  Each of these is a value for one named register, so
// they stay separate constants rather than becoming an enum: nothing ever
// holds "one of these", and the border width is a pixel count, not a bit
// pattern.

// VICIV.addr ($D018) in its C64 form: charset at $1000, the uppercase ROM set.
constexpr uint8_t VIC4_ADDR_UPPERCASE = 0x15;

// VICIV.sdbdrwd_lsb/_msb ($D05C/$D05D).  The width is 12 bits across the pair,
// but bit 7 of the MSB is HOTREG, the switch that makes legacy VIC-II/III
// register writes propagate to their VIC-IV equivalents.  Bit 6 is set here
// and is not documented in the user guide.
constexpr uint8_t VIC4_SIDE_BORDER_WIDTH = 80;
constexpr uint8_t VIC4_BORDER_MSB_HOTREG = 0xC0;

// VICIV.ctrlb ($D031).
constexpr uint8_t VIC4_CTRLB_80_COLUMN = 0xE0;
constexpr uint8_t VIC4_CTRLB_EXTENDED_ATTRIBUTES = 0x20;

// VICIV.ctrlc ($D054).  The masks keep the bits a caller must not disturb --
// CRT emulation among them -- while setting the mode in the low bits.
constexpr uint8_t VIC4_CTRLC_LEGACY_MASK = 0xF8;
constexpr uint8_t VIC4_CTRLC_MODE_MASK = 0xA8;
constexpr uint8_t VIC4_CTRLC_16BIT_FULL_COLOUR = 0x05;

// VICIV.chrxscl ($D05A), the horizontal character scale.
constexpr uint8_t VIC4_CHRXSCL_80_COLUMN = 0x78;

// $00/$01 CPU port: all lines driven, BASIC banked out, KERNAL and I/O kept.
constexpr uint8_t CPU_PORT_DDR_ALL_OUTPUTS = 0x3F;
constexpr uint8_t CPU_PORT_KERNAL_AND_IO = 0x36;

// VIC bank select lives in the low two bits of CIA2 port A, inverted, so %01
// selects $8000-$BFFF.
constexpr uint8_t CIA2_VIC_BANK_MASK = 0xFC;
constexpr uint8_t CIA2_VIC_BANK_8000 = 0x01;
constexpr uint8_t CIA_ICR_DISABLE_ALL = 0x7F;

// Keyboard event queue: reads the top event as ASCII, 0x00 when empty;
// assigning any value dequeues.  See the user guide, "Keyboard".
#define ASCIIKEY MMIO8(0xD610)

// UARTMISC modifier key state, read live rather than through the queue: a bit
// is set while its key is held.
#define MODKEY MMIO8(0xD611)
constexpr uint8_t MODKEY_LSHIFT = 0x01; // UARTMISC:MLSHFT
constexpr uint8_t MODKEY_RSHIFT = 0x02; // UARTMISC:MRSHFT
constexpr uint8_t MODKEY_CTRL = 0x04;   // UARTMISC:MCTRL
constexpr uint8_t MODKEY_MEGA = 0x08;   // UARTMISC:MMEGA

// UARTMISC joystick options.
#define UART_MISC MMIO8(0xD612)
constexpr uint8_t UART_MISC_OSKDEBUG = 0x10; // UARTMISC:OSKDEBUG, write only
constexpr uint8_t UART_MISC_JOYSWAP = 0x20;  // UARTMISC:JOYSWAP

// UARTMISC:M65MODEL, the board this core is running on.
#define M65MODEL MMIO8(0xD629)
