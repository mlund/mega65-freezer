/*
  Full-RAM access for a program confined to the first 64KB: everything goes
  through the DMAgic rather than through banking.
*/

#include "fdisk_memory.h"

#include "fdisk_screen.h"
#include "mega65.inc"

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
    unsigned int count;
    unsigned int source_addr;
    unsigned char source_bank;
    unsigned int dest_addr;
    unsigned char dest_bank;
    unsigned char sub_cmd; // F018B subcmd
    unsigned int modulo;
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
    POKE(0xd701U, ((unsigned int)&dmalist) >> 8);
    POKE(0xd705U, ((unsigned int)&dmalist) & 0xff); // triggers enhanced DMA
}

__attribute__((noinline)) void lcopy(
    long source_address, long destination_address, unsigned int count) {
    if (!count)
        return;
    dmalist.option_0b = 0x0b;
    dmalist.option_80 = 0x80;
    dmalist.source_mb = source_address >> 20;
    dmalist.option_81 = 0x81;
    dmalist.dest_mb = (destination_address >> 20);
    dmalist.end_of_options = 0x00;

    dmalist.command = 0x00; // copy
    dmalist.count = count;
    dmalist.sub_cmd = 0;
    dmalist.source_addr = source_address & 0xffff;
    dmalist.source_bank = (source_address >> 16) & 0x0f;
    if (source_address >= 0xd000 && source_address < 0xe000)
        dmalist.source_bank |= 0x80;
    dmalist.dest_addr = destination_address & 0xffff;
    dmalist.dest_bank = (destination_address >> 16) & 0x0f;
    if (destination_address >= 0xd000 && destination_address < 0xe000)
        dmalist.dest_bank |= 0x80;

    do_dma();
    return;
}

__attribute__((noinline)) void lfill(
    long destination_address, unsigned char value, unsigned int count) {
    if (!count)
        return;
    dmalist.option_0b = 0x0b;
    dmalist.option_80 = 0x80;
    dmalist.source_mb = 0x00;
    dmalist.option_81 = 0x81;
    dmalist.dest_mb = destination_address >> 20;
    dmalist.end_of_options = 0x00;

    dmalist.command = 0x03; // fill
    dmalist.sub_cmd = 0;
    dmalist.count = count;
    dmalist.source_addr = value;
    dmalist.dest_addr = destination_address & 0xffff;
    dmalist.dest_bank = (destination_address >> 16) & 0x0f;
    if (destination_address >= 0xd000 && destination_address < 0xe000)
        dmalist.dest_bank |= 0x80;

    do_dma();
    return;
}

void m65_io_enable(void) {
    // Gate C65 IO enable
    VICIV.key = VIC4_KNOCK_1;
    VICIV.key = VIC4_KNOCK_2;
    // Force to full speed
    POKE(0, 65);
}
