/*
  Full-RAM access for a program confined to the first 64KB: everything goes
  through the DMAgic rather than through banking.
*/

#include "dma.h"

#include "mega65_regs.h"

#include <mega65.h>

struct DmagicDmalist {
    // Enhanced DMA options
    unsigned char option_0b;
    unsigned char option_80;
    unsigned char source_mb;
    unsigned char option_81;
    unsigned char dest_mb;
    /* The destination step.  One list rather than two: a step of 1 is what the
     * DMAgic defaults to, so the plain copy and fill below are the strided ones
     * with the step set to 1. */
    unsigned char option_85;
    unsigned char dest_skip;
    unsigned char end_of_options;

    // F018B format DMA request
    unsigned char command;
    uint16_t count;
    uint16_t source_addr;
    unsigned char source_bank;
    uint16_t dest_addr;
    unsigned char dest_bank;
    unsigned char sub_cmd; // F018B subcmd
    uint16_t modulo;
};

/* The DMAgic is the only reader of dmalist, so without volatile the compiler
 * drops a population as dead when the next call overwrites it, and the pending
 * job runs on the stale list. */
static volatile struct DmagicDmalist dmalist;

__attribute__((noinline)) void do_dma(void) {
    m65_io_enable();
    /* volatile covers dmalist; this covers the caller's buffers, which the
     * DMAgic also touches. */
    __asm__ volatile("" ::: "memory");

    // Now run DMA job (to and from anywhere, and list is in low 1MB)
    DMA.addr_bank = 0;
    DMA.addr_mb = 0x00; // list is in the first megabyte
    DMA.addr_msb = ((uint16_t)&dmalist) >> 8;
    DMA.trigger_enhanced = ((uint16_t)&dmalist) & 0xff; // writing the low byte triggers it
}

/* Everything both jobs share; the caller sets command, source and count. */
static void dmalist_prepare(Addr28 destination_address, uint8_t skip) {
    dmalist.option_0b = 0x0b;
    dmalist.option_80 = 0x80;
    dmalist.option_81 = 0x81;
    dmalist.dest_mb = (uint8_t)(destination_address >> 20);
    dmalist.option_85 = 0x85;
    dmalist.dest_skip = skip;
    dmalist.end_of_options = 0x00;

    dmalist.sub_cmd = 0;
    dmalist.modulo = 0;
    dmalist.dest_addr = destination_address & 0xffff;
    dmalist.dest_bank = (destination_address >> 16) & 0x0f;
    /* Bit 7 selects I/O rather than RAM. */
    if (destination_address >= 0xd000 && destination_address < 0xe000) {
        dmalist.dest_bank |= 0x80;
    }
}

__attribute__((noinline)) void lcopy_skip(
    Addr28 source_address, Addr28 destination_address, uint16_t count, uint8_t skip) {
    if (!count) {
        return;
    }
    dmalist_prepare(destination_address, skip);
    dmalist.command = 0x00; // copy
    dmalist.count = count;
    dmalist.source_mb = (uint8_t)(source_address >> 20);
    dmalist.source_addr = source_address & 0xffff;
    dmalist.source_bank = (source_address >> 16) & 0x0f;
    if (source_address >= 0xd000 && source_address < 0xe000) {
        dmalist.source_bank |= 0x80;
    }

    do_dma();
}

__attribute__((noinline)) void lfill_skip(
    Addr28 destination_address, uint8_t value, uint16_t count, uint8_t skip) {
    if (!count) {
        return;
    }
    dmalist_prepare(destination_address, skip);
    dmalist.command = 0x03; // fill
    dmalist.count = count;
    dmalist.source_mb = 0x00;
    dmalist.source_bank = 0x00;
    dmalist.source_addr = value;

    do_dma();
}

void lcopy(Addr28 source_address, Addr28 destination_address, uint16_t count) {
    lcopy_skip(source_address, destination_address, count, 1);
}

__attribute__((noinline)) void lcopy_near(const void* from, void* to, uint16_t count) {
    lcopy_skip((Addr28)(uint16_t)from, (Addr28)(uint16_t)to, count, 1);
}

__attribute__((noinline)) void lcopy_out(const void* from, Addr28 to, uint16_t count) {
    lcopy_skip((Addr28)(uint16_t)from, to, count, 1);
}

__attribute__((noinline)) void lcopy_in(Addr28 from, void* to, uint16_t count) {
    lcopy_skip(from, (Addr28)(uint16_t)to, count, 1);
}

void lfill(Addr28 destination_address, uint8_t value, uint16_t count) {
    lfill_skip(destination_address, value, count, 1);
}

void m65_io_enable(void) {
    // Gate C65 IO enable
    VICIV.key = VIC4_KEY_VICIV_A;
    VICIV.key = VIC4_KEY_VICIV_B;
    CPU_PORTDDR = CPU_PORT_FORCE_FAST;
}
