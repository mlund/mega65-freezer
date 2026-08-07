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
 * job runs on the stale list.  dma_byte is the reverse: the DMAgic writes it
 * during lpeek(). */
volatile struct DmagicDmalist dmalist;
volatile unsigned char dma_byte;

__attribute__((noinline)) void do_dma(void) {
    m65_io_enable();
    /* volatile covers dmalist; this covers the caller's buffers, which the
     * DMAgic also touches. */
    __asm__ volatile("" ::: "memory");

    // Now run DMA job (to and from anywhere, and list is in low 1MB)
    POKE(0xd702U, 0);
    POKE(0xd704U, 0x00); // List is in $00xxxxx
    POKE(0xd701U, ((uint16_t)&dmalist) >> 8);
    POKE(0xd705U, ((uint16_t)&dmalist) & 0xff); // triggers enhanced DMA
}

__attribute__((noinline)) void lcopy(
    Addr28 source_address, Addr28 destination_address, uint16_t count) {
    if (!count) {
        return;
    }
    dmalist.option_0b = 0x0b;
    dmalist.option_80 = 0x80;
    dmalist.source_mb = (uint8_t)(source_address >> 20);
    dmalist.option_81 = 0x81;
    dmalist.dest_mb = (uint8_t)(destination_address >> 20);
    dmalist.end_of_options = 0x00;

    dmalist.command = 0x00; // copy
    dmalist.count = count;
    dmalist.sub_cmd = 0;
    dmalist.source_addr = source_address & 0xffff;
    dmalist.source_bank = (source_address >> 16) & 0x0f;
    if (source_address >= 0xd000 && source_address < 0xe000) {
        dmalist.source_bank |= 0x80;
    }
    dmalist.dest_addr = destination_address & 0xffff;
    dmalist.dest_bank = (destination_address >> 16) & 0x0f;
    if (destination_address >= 0xd000 && destination_address < 0xe000) {
        dmalist.dest_bank |= 0x80;
    }

    do_dma();
}

__attribute__((noinline)) void lfill(
    Addr28 destination_address, unsigned char value, uint16_t count) {
    if (!count) {
        return;
    }
    dmalist.option_0b = 0x0b;
    dmalist.option_80 = 0x80;
    dmalist.source_mb = 0x00;
    dmalist.option_81 = 0x81;
    dmalist.dest_mb = (uint8_t)(destination_address >> 20);
    dmalist.end_of_options = 0x00;

    dmalist.command = 0x03; // fill
    dmalist.sub_cmd = 0;
    dmalist.count = count;
    dmalist.source_addr = value;
    dmalist.dest_addr = destination_address & 0xffff;
    dmalist.dest_bank = (destination_address >> 16) & 0x0f;
    if (destination_address >= 0xd000 && destination_address < 0xe000) {
        dmalist.dest_bank |= 0x80;
    }

    do_dma();
}

/* A second list, because the plain one has no room for the skip-rate option and
 * carrying it there would tax the five tools that never stride.  Only the
 * destination rate ($85) is needed: the source is read consecutively in both
 * jobs, which is the DMAgic's default. */
struct DmagicSkipList {
    unsigned char option_0b;
    unsigned char option_80;
    unsigned char source_mb;
    unsigned char option_81;
    unsigned char dest_mb;
    unsigned char option_85;
    unsigned char dest_skip;
    unsigned char end_of_options;

    unsigned char command;
    uint16_t count;
    uint16_t source_addr;
    unsigned char source_bank;
    uint16_t dest_addr;
    unsigned char dest_bank;
    unsigned char sub_cmd;
    uint16_t modulo;
};

static volatile struct DmagicSkipList skiplist;

__attribute__((noinline)) static void do_skip_dma(void) {
    m65_io_enable();
    __asm__ volatile("" ::: "memory");

    POKE(0xd702U, 0);
    POKE(0xd704U, 0x00);
    POKE(0xd701U, ((uint16_t)&skiplist) >> 8);
    POKE(0xd705U, ((uint16_t)&skiplist) & 0xff);
}

/* Fills in what both jobs share; the caller sets command, source and count. */
static void skiplist_prepare(Addr28 destination_address, uint8_t skip) {
    skiplist.option_0b = 0x0b;
    skiplist.option_80 = 0x80;
    skiplist.source_mb = 0x00;
    skiplist.option_81 = 0x81;
    skiplist.dest_mb = (uint8_t)(destination_address >> 20);
    skiplist.option_85 = 0x85;
    skiplist.dest_skip = skip;
    skiplist.end_of_options = 0x00;

    skiplist.sub_cmd = 0;
    skiplist.modulo = 0;
    skiplist.dest_addr = destination_address & 0xffff;
    skiplist.dest_bank = (destination_address >> 16) & 0x0f;
}

__attribute__((noinline)) void lfill_skip(
    Addr28 destination_address, uint8_t value, uint16_t count, uint8_t skip) {
    if (!count) {
        return;
    }
    skiplist_prepare(destination_address, skip);
    skiplist.command = 0x03; // fill
    skiplist.count = count;
    skiplist.source_addr = value;

    do_skip_dma();
}

__attribute__((noinline)) void lcopy_skip(
    Addr28 source_address, Addr28 destination_address, uint16_t count, uint8_t skip) {
    if (!count) {
        return;
    }
    skiplist_prepare(destination_address, skip);
    skiplist.command = 0x00; // copy
    skiplist.count = count;
    skiplist.source_mb = (uint8_t)(source_address >> 20);
    skiplist.source_addr = source_address & 0xffff;
    skiplist.source_bank = (source_address >> 16) & 0x0f;

    do_skip_dma();
}

void m65_io_enable(void) {
    // Gate C65 IO enable
    VICIV.key = VIC4_KNOCK_1;
    VICIV.key = VIC4_KNOCK_2;
    // Force to full speed
    POKE(0, 65);
}
