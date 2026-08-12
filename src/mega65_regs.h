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

// A register named this way is an lvalue, so it reads and assigns directly
// instead of through PEEK/POKE.  A macro because it has to be assignable; the
// plain values above are constexpr so the compiler checks their type.
#define REG8(addr) (*(volatile uint8_t*)(addr))

// The VIC-III/IV conceals its registers until this knock is written to
// VICIV.key ($D02F), in this order.
constexpr uint8_t VIC4_KNOCK_1 = 0x47;
constexpr uint8_t VIC4_KNOCK_2 = 0x53;

// F011 floppy controller (45IO27).  See the MEGA65 user guide, "45IO27
// Multi-Function I/O Controller".
#define F011_CONTROL REG8(0xD080) // motor and drive select
#define F011_COMMAND REG8(0xD081)
#define F011_STATUS REG8(0xD082)           // BUSY is bit 7
#define F011_TRACK REG8(0xD084)            // FDC:TRACK
#define F011_SECTOR REG8(0xD085)           // FDC:SECTOR
#define F011_SIDE REG8(0xD086)             // FDC:SIDE
#define F011_DATA REG8(0xD087)             // sector byte port
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

// Read through lpeek rather than REG8: these are 28-bit addresses, reachable
// whatever is banked at $D000.  Names from mega65-core's iomap.txt.
constexpr uint32_t M65_MODEL_ID = 0xffd3629; // UARTMISC:M65MODEL, the board revision
constexpr uint32_t RTC_SECONDS = 0xffd7110;  // RTC:RTCSEC; minutes, hours, day,
                                             // month and year follow in order

// DMAgic.  Writing DMA_ENABLE is what starts the job, so it goes last; the
// other three only say where the list is.
#define DMA_ADDR_MSB REG8(0xD701)
#define DMA_ADDR_BANK REG8(0xD702)
#define DMA_ADDR_MB REG8(0xD704)
#define DMA_ENABLE REG8(0xD705)

// SD controller.  SD:CMDANDSTAT takes a command when written and reports
// status when read; bit and command names from mega65-core's iomap.txt and the
// $D680 write decode in sdcardio.vhdl.
#define SD_COMMAND REG8(0xD680)
#define SD_STATUS REG8(0xD680) // same register; reads status

constexpr uint8_t SD_STATUS_SDIO_BUSY = 0b00000001;
constexpr uint8_t SD_STATUS_CARD_BUSY = 0b00000010;
constexpr uint8_t SD_STATUS_BUSY = 0b00000011; // either engine still working
constexpr uint8_t SD_STATUS_SDHC = 0b00010000;
constexpr uint8_t SD_STATUS_ERROR = 0b01000000; // reported in bit 6 so V can test it
/* Busy, still in reset, or in error -- all of which must be clear before a
 * command's result means anything. */
constexpr uint8_t SD_STATUS_UNSETTLED = 0b01100111;

constexpr uint8_t SD_CMD_RESET_BEGIN = 0x00;
constexpr uint8_t SD_CMD_RESET_END = 0x01;
constexpr uint8_t SD_CMD_READ = 0x02;
constexpr uint8_t SD_CMD_WRITE = 0x03;
constexpr uint8_t SD_CMD_WRITE_MULTI_FIRST = 0x04;
constexpr uint8_t SD_CMD_WRITE_MULTI_NEXT = 0x05;
constexpr uint8_t SD_CMD_WRITE_MULTI_LAST = 0x06;
constexpr uint8_t SD_CMD_SDHC_MODE = 0x41;
constexpr uint8_t SD_CMD_WRITE_GATE = 0x57; // opens a window of about 1ms

// SD:SDSECTOR0-3, low byte first.  Written a byte at a time rather than as one
// 32-bit store, so the order the controller sees is the order written here.
#define SD_SECTOR_ADDR(byte) REG8(0xD681 + (byte))

// $D689 carries several unrelated signals; only the buffer select is used
// here.  SD:BUFSEL, 1 = the SD card's sector buffer, 0 = the F011/FDC's.
#define SD_MISC REG8(0xD689)
constexpr uint8_t SD_MISC_BUFSEL_SDCARD = 0x80;

// F011 disk-image type and control.  Bit 6 is drive 0 and bit 7 drive 1 in
// both, so the code shifts by the drive id.
#define SDFDC_IMAGE_TYPE REG8(0xD68A) // SDFDC:D0D64 / D1D64, set for D64
#define SDFDC_CONTROL REG8(0xD68B)    // SDFDC:D0MD / D1MD, set for D65

// AUDIOMIX:REGSEL selects a mixer coefficient, which is then read or written
// through AUDIOMIX:REGWDATA.
#define AUDIOMIX_REGSEL REG8(0xD6F4)
#define AUDIOMIX_REGDATA REG8(0xD6F5)

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
/* The CRT emulation the user chose, which is theirs to keep: a tool that
 * resets the display carries this bit across rather than imposing its own. */
constexpr uint8_t VIC4_CTRLC_CRT_EMULATION = 0x20;

// VICIV.rasline0 ($D06F).
/* Set for NTSC, clear for PAL, and the two need different border and sprite
 * positions -- so it decides more than the frame rate. */
constexpr uint8_t VIC4_RASLINE0_NTSC = 0x80;

// VICIV.chrxscl ($D05A), the horizontal character scale.
constexpr uint8_t VIC4_CHRXSCL_80_COLUMN = 0x78;

// $00/$01 CPU port: all lines driven, BASIC banked out, KERNAL and I/O kept.
constexpr uint8_t CPU_PORT_DDR_ALL_OUTPUTS = 0x3F;
constexpr uint8_t CPU_PORT_KERNAL_AND_IO = 0x36;

// The SDK names the two registers; these are the values.  CPU_PORTDDR is a
// direction register for every value but two: the CPU intercepts $40 and $41
// and they set the speed instead, never reaching the DDR (mega65-core,
// src/vhdl/gs4510.vhdl:3301).
constexpr uint8_t CPU_PORT_FORCE_FAST = 0x41;
constexpr uint8_t CPU_PORT_FORCE_FAST_OFF = 0x40;

// VIC bank select lives in the low two bits of CIA2 port A, inverted, so %01
// selects $8000-$BFFF.
constexpr uint8_t CIA2_VIC_BANK_MASK = 0xFC;
constexpr uint8_t CIA2_VIC_BANK_8000 = 0x01;
constexpr uint8_t CIA_ICR_DISABLE_ALL = 0x7F;

// Keyboard event queue: reads the top event as ASCII, 0x00 when empty;
// assigning any value dequeues.  See the user guide, "Keyboard".
#define ASCIIKEY REG8(0xD610)

/* The codes ASCIIKEY reports for the keys the tools dispatch on.  The commands
 * are function keys because a letter is typed text in the monitor's line
 * editor; a shifted function key is the next code up, so F2 is KEY_F1 + 1. */
constexpr uint8_t KEY_RUN_STOP = 0x03;
constexpr uint8_t KEY_RETURN = 0x0D;
constexpr uint8_t KEY_CURSOR_DOWN = 0x11;
constexpr uint8_t KEY_HOME = 0x13;
constexpr uint8_t KEY_DELETE = 0x14;
constexpr uint8_t KEY_ESC = 0x1B;
constexpr uint8_t KEY_CURSOR_RIGHT = 0x1D;
constexpr uint8_t KEY_HELP = 0x1F;
constexpr uint8_t KEY_LEFT_ARROW = 0x5F; // the one above CTRL, not a cursor key
constexpr uint8_t KEY_CURSOR_UP = 0x91;
constexpr uint8_t KEY_SHIFT_HOME = 0x93;
constexpr uint8_t KEY_CURSOR_LEFT = 0x9D;
constexpr uint8_t KEY_F1 = 0xF1;
constexpr uint8_t KEY_F3 = 0xF3;
constexpr uint8_t KEY_F4 = 0xF4;
constexpr uint8_t KEY_F5 = 0xF5;
constexpr uint8_t KEY_F7 = 0xF7;
constexpr uint8_t KEY_F9 = 0xF9;
constexpr uint8_t KEY_F11 = 0xFB;
constexpr uint8_t KEY_F14 = 0xFE;

// UARTMISC modifier key state, read live rather than through the queue: a bit
// is set while its key is held.
#define MODKEY REG8(0xD611)
constexpr uint8_t MODKEY_LSHIFT = 0x01; // UARTMISC:MLSHFT
constexpr uint8_t MODKEY_RSHIFT = 0x02; // UARTMISC:MRSHFT
constexpr uint8_t MODKEY_CTRL = 0x04;   // UARTMISC:MCTRL
constexpr uint8_t MODKEY_MEGA = 0x08;   // UARTMISC:MMEGA

// UARTMISC joystick options.
#define UART_MISC REG8(0xD612)
constexpr uint8_t UART_MISC_OSKDEBUG = 0x10; // UARTMISC:OSKDEBUG, write only
constexpr uint8_t UART_MISC_JOYSWAP = 0x20;  // UARTMISC:JOYSWAP

// UARTMISC:M65MODEL, the board this core is running on.
#define M65MODEL REG8(0xD629)
