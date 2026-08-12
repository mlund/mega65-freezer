#include "cpumap.h"

static uint16_t frozen_map_lo = 0;
static uint16_t frozen_map_hi = 0;
static uint8_t frozen_map_lo_megabyte = 0;
static uint8_t frozen_map_hi_megabyte = 0;
static uint8_t frozen_cpu_port = 0;
static uint8_t frozen_cpu_port_ddr = 0;
static uint8_t frozen_rom_banking = 0;

void cpumap_load(const uint8_t* saved, uint8_t rom_banking) {
    /* $D64A/$D64C carry the selection nibble and the offset's high nibble,
     * $D64B/$D64D the offset's low byte -- high byte first, not the little
     * endian the rest of the save area uses. */
    frozen_map_lo = (uint16_t)((uint16_t)saved[0] << 8) | saved[1];
    frozen_map_hi = (uint16_t)((uint16_t)saved[2] << 8) | saved[3];
    frozen_map_lo_megabyte = saved[4];
    frozen_map_hi_megabyte = saved[5];
    frozen_cpu_port_ddr = saved[6];
    frozen_cpu_port = saved[7];
    frozen_rom_banking = rom_banking;
}

/* The C65 ROM banks sit $20000 above the 16-bit windows they serve, so every
 * ROM answer here is the address plus this. */
constexpr Addr28 ROM_BANK_OFFSET = 0x20000UL;

/* The register set every I/O personality aliases, and the one the slot saves. */
constexpr Addr28 IO_PAGE = 0xFFD3000UL;

/* $DE00-$DFFF reaches the cartridge port rather than the I/O page. */
constexpr Addr28 CARTRIDGE_IO = 0x7FFD000UL;
constexpr uint16_t CARTRIDGE_IO_FIRST = 0xDE00;

/* $D030 bit 0. Within address resolution its only effect is to keep
 * $DE00-$DFFF on the I/O page; routing $DC00 to colour RAM happens downstream
 * in the iomapper, which never changes the address the CPU formed. */
constexpr uint8_t ROM_BANKING_CRAM2K = 0x01;

/* The four VIC-III banking bits, each naming the window it covers. */
constexpr uint8_t ROM_BANKING_ROM8 = 0x08;
constexpr uint8_t ROM_BANKING_ROMA = 0x10;
constexpr uint8_t ROM_BANKING_ROMC = 0x20;
constexpr uint8_t ROM_BANKING_ROME = 0x80;

/* Which bit banks each 4KB window from $8000 up, and what to call it.  ROMC
 * covers only $C000-$CFFF, so $D000 gets a bit no register can set. */
static const uint8_t ROM_BANKING_WINDOW[8] = {ROM_BANKING_ROM8, ROM_BANKING_ROM8,
    ROM_BANKING_ROMA, ROM_BANKING_ROMA, ROM_BANKING_ROMC, 0x00, ROM_BANKING_ROME,
    ROM_BANKING_ROME};

static const CpuMapMechanism ROM_BANKING_MECHANISM[8] = {CpuMapRom8, CpuMapRom8, CpuMapRomA,
    CpuMapRomA, CpuMapRomC, CpuMapRam, CpuMapRomE, CpuMapRomE};

/* Character ROM as read through $D000, and the RAM a write to it reaches. */
constexpr Addr28 CHARACTER_ROM = 0x0002D000UL;
constexpr Addr28 RAM_UNDER_IO = 0x0000D000UL;

/* Which branch below settled the last call.  A side channel and not an out
 * parameter: only cpumap_run() wants it, and the argument would cost bytes at
 * every call site to carry a value the others discard. */
static CpuMapMechanism decided_by = CpuMapRam;

Addr28 resolve_cpu_address(uint16_t cpu_address, bool writing) {
    unsigned char block = (unsigned char)(cpu_address >> 13);
    bool lower_half = block < 4;
    uint16_t map = lower_half ? frozen_map_lo : frozen_map_hi;
    unsigned char nibble;
    unsigned char lines;
    Addr28 address;

    /* MAP is the only mechanism that answers outright: gs4510.vhdl returns
     * here, and everything below it only edits an address that started as the
     * 16-bit one.  That structure is what decides the priorities -- the later
     * a mechanism appears, the more it overrides. */
    if (((map >> 12) & (1U << (block & 3))) != 0) {
        unsigned char megabyte = lower_half ? frozen_map_lo_megabyte : frozen_map_hi_megabyte;

        decided_by = lower_half ? CpuMapMapLow : CpuMapMapHigh;
        /* The offset addition wraps inside its megabyte, which the 45GS02's
         * own megabyte register then places in the 28-bit space. */
        return ((Addr28)megabyte << 20) |
            (((Addr28)cpu_address + (((Addr28)map & 0x0FFF) << 8)) & 0xFFFFFL);
    }

    nibble = (unsigned char)(cpu_address >> 12);
    address = cpu_address;
    decided_by = CpuMapRam;

    /* C64-style banking. A line driven as input floats high, so the DDR forces
     * the bit rather than merely gating it. */
    lines = (unsigned char)((frozen_cpu_port | (unsigned char)~frozen_cpu_port_ddr) & 0x07);

    if (nibble == 0x0D) {
        if (lines >= 0x05) {
            /* Of the four conditions that keep the expansion area off the
             * cartridge port, CRAM2K is the only one a freeze slot records:
             * the SD sector buffer mapping is stashed at $FFD6000 rather than
             * left at $D680, and neither the ethernet personality nor a
             * cartridge's own mode is saved at all. */
            if (cpu_address >= CARTRIDGE_IO_FIRST &&
                (frozen_rom_banking & ROM_BANKING_CRAM2K) == 0) {
                address = CARTRIDGE_IO | (cpu_address & 0x0FFF);
                decided_by = CpuMapCram2k;
            } else {
                address = IO_PAGE | (cpu_address & 0x0FFF);
                decided_by = CpuMapPort;
            }
        } else if (lines != 0x00 && lines != 0x04) {
            address = (writing ? RAM_UNDER_IO : CHARACTER_ROM) | (cpu_address & 0x0FFF);
            decided_by = CpuMapPort;
        } else {
            /* The port chose plain RAM, so nothing banked this window. */
            address = RAM_UNDER_IO | (cpu_address & 0x0FFF);
        }
    }

    /* The ROMs answer reads only; a write falls through to the RAM beneath,
     * exactly as it does on a C64.  Bit 1 banks the KERNAL in, bits 0+1
     * together the BASIC ROM, both $20000 above their 16-bit window. */
    if (!writing) {
        if ((nibble >= 0x0E && (lines & 0x02) != 0) ||
            (nibble >= 0x0A && nibble < 0x0C && (lines & 0x03) == 0x03)) {
            address = ROM_BANK_OFFSET + cpu_address;
            decided_by = CpuMapPort;
        }

        /* $D030 last, so it overrides $01 wherever both claim a window.  One
         * lookup rather than four tests: the windows are exclusive, and $D000
         * is covered by a zero bit that can never match. */
        if (nibble >= 0x08) {
            unsigned char window = (unsigned char)(nibble - 0x08);

            if ((frozen_rom_banking & ROM_BANKING_WINDOW[window]) != 0) {
                address = ROM_BANK_OFFSET + cpu_address;
                decided_by = ROM_BANKING_MECHANISM[window];
            }
        }
    }

    return address;
}

Addr28 cpumap_typed_address(uint32_t typed) {
    if ((typed & CPU_VIEW_FLAG) != 0) {
        return resolve_cpu_address((uint16_t)typed, false) & 0x0FFFFFFFUL;
    }
    return typed & 0x0FFFFFFFUL;
}

/* The coarsest step that cannot step over a boundary.  The mechanisms divide
 * the space at 8KB (MAP), 4KB ($D030 and the I/O window) and 512 bytes (the
 * cartridge expansion area at $DE00) -- but a MAP offset is a multiple of $100
 * and its 20-bit sum wraps at whatever $100 boundary it reaches, so only a
 * 256-byte walk sees every place a run can end. */
constexpr uint16_t WINDOW_SIZE = 0x100;

void cpumap_run(uint16_t from, CpuMapRun* run) {
    run->first = from;
    run->target = resolve_cpu_address(from, false);
    run->by = decided_by;
    run->last = (uint16_t)(from + WINDOW_SIZE - 1);

    /* Absorb the next window while it neither changes mechanism nor breaks the
     * run of destination addresses.  Carrying the expected target forward keeps
     * that test to one 32-bit add per window. */
    Addr28 expected = run->target;

    while (run->last != 0xFFFF) {
        expected += WINDOW_SIZE;
        if (resolve_cpu_address((uint16_t)(run->last + 1), false) != expected ||
            decided_by != run->by) {
            return;
        }
        run->last = (uint16_t)(run->last + WINDOW_SIZE);
    }
}
