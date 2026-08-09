/*
 * Exercises the freeze slot round trip: freeze_poke/freeze_peek a byte at a
 * time, and the whole-sector freeze_store_sector_partial/
 * freeze_fetch_sector_partial pair that batching uses instead.
 *
 * Nothing on the host can reach these.  The slot is a run of SD sectors whose
 * start comes from a hypervisor trap, and every access goes through the SD
 * controller, so it needs a machine or an emulator with a card.
 *
 * Run by CTest under Xemu against a cloned card, so the image passed in is
 * never written to.  Each check has a number and failure quits the emulator
 * with it, so the exit status says which one went.  Zero means all passed.
 *
 * The scratch area is the process descriptor's disk-image name field.  It is
 * the region batching is aimed at, it is one sector by hyppo's own region
 * table (test/verify_slotmap.py pins that), and with no frozen program on the
 * card nothing reads back what is written here.
 */

#include "common.h"
#include "mega65_regs.h"
#include "sdcard.h"
#include "slot.h"
#include "slotmap.h"

#include <mega65/debug.h>

/* Writing a code here and then XEMU_QUIT ends the run with that code.  Needs
 * Xemu's -testing; on hardware the write lands in an unused register and the
 * program simply carries on. */
constexpr uint16_t XEMU_CONTROL = 0xD6CF;
constexpr uint8_t XEMU_QUIT = 0x42;

/* The frozen process descriptor's second disk-image name, 32 bytes at $35 into
 * a structure hyppo places at $FFFBD00.  src/slot.h asserts that offset. */
constexpr uint32_t SCRATCH = 0xFFFBD00UL + 0x35;
constexpr uint8_t SCRATCH_BYTES = 32;
/* The region's own base, where a store needs no read-before-write, and where
 * SCRATCH begins within its sector. */
constexpr uint32_t SCRATCH_BASE = 0xFFFBD00UL;
constexpr uint8_t SCRATCH_OFFSET = 0x35;

[[noreturn]] static void finish(uint8_t code) {
    POKE(XEMU_CONTROL, code);
    POKE(XEMU_CONTROL, XEMU_QUIT);
    for (;;) {
        __asm__ volatile("");
    }
}

static void check(uint8_t number, uint8_t got, uint8_t want) {
    if (got != want) {
        debug_msg("FAIL");
        finish(number);
    }
}

static unsigned char buffer[SCRATCH_BYTES];
/* A whole sector, to compare one way of writing a field against another. */
static unsigned char snapshot[512];

int main(void) {
    mega65_fast();
    __asm__ volatile("sei" ::: "memory");

    /* The same opening the seven tools do.  sdhc_card in particular: without
     * it sdcard.c addresses the card in bytes rather than sectors, and the
     * controller never reports the transfer complete. */
    POKE(0x00, CPU_PORT_DDR_ALL_OUTPUTS);
    POKE(0x01, CPU_PORT_KERNAL_AND_IO);
    sdhc_card = (SD_STATUS & SD_STATUS_SDHC) != 0;

    request_freeze_region_list();
    freeze_slot_start_sector = read_freeze_slot_start_sector(0);

    /* 1: the hypervisor answered at all.  Everything below is meaningless
     * without a region list, and a zero count would make each check pass by
     * writing and reading nothing. */
    check(1, freeze_region_count > 0, 1);

    /* 2: the scratch field is in the slot, so the writes below have somewhere
     * to land rather than being silently dropped. */
    check(2, address_to_freeze_slot_offset(SCRATCH) != FREEZE_SLOT_NOT_PRESENT, 1);

    /* 3: the whole field is one sector, which is what lets a caller replace 32
     * bytes with one read and one write instead of 32 of each. */
    check(3,
        (address_to_freeze_slot_offset(SCRATCH) >> 9) ==
            (address_to_freeze_slot_offset(SCRATCH + SCRATCH_BYTES - 1) >> 9),
        1);

    /* 4: a byte written one at a time reads back one at a time.  The value
     * varies per index so a stuck address shows as a wrong byte, not a match. */
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        freeze_poke(SCRATCH + i, (unsigned char)(i ^ 0xA5));
    }
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        check(4, freeze_peek(SCRATCH + i), (unsigned char)(i ^ 0xA5));
    }

    /* 5: the batched write lands where the byte-at-a-time reader finds it.
     * This is the pair batching replaces the loop above with, so the two
     * disagreeing is exactly the failure that would otherwise ship. */
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        buffer[i] = (unsigned char)(i + 0x40);
    }
    check(5, freeze_store_sector_partial(SCRATCH, (uint32_t)buffer, SCRATCH_BYTES), FreezerOk);
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        check(5, freeze_peek(SCRATCH + i), (unsigned char)(i + 0x40));
    }

    /* 6: and the batched read returns what the byte-at-a-time writer left,
     * which is the other half of the substitution. */
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        freeze_poke(SCRATCH + i, (unsigned char)(0xFF - i));
        buffer[i] = 0;
    }
    check(6, freeze_fetch_sector_partial(SCRATCH, (uint32_t)buffer, SCRATCH_BYTES), FreezerOk);
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        check(6, buffer[i], (unsigned char)(0xFF - i));
    }

    /* 7: a write outside every region is refused rather than landing somewhere
     * arbitrary.  $FFD5000 is between two regions in hyppo's table. */
    check(7, freeze_peek(0xFFD5000UL), FreezerNotFrozen);

    /* 8: freeze_fetch_sector with a null buffer leaves the sector in
     * sector_buffer instead of copying it out, and freeze_store_sector with a
     * null buffer writes that back.  At a region's base the store skips its
     * read-before-write, so the pair is two transfers for a whole sector --
     * the cheapest way to change several bytes of one.  Nothing else pins
     * these two forms; checks 5 and 6 cover only the partial ones. */
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        freeze_poke(SCRATCH + i, (unsigned char)(i ^ 0x3C));
    }
    check(8, freeze_fetch_sector(SCRATCH_BASE, NULL), FreezerOk);
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        check(8, sector_buffer[SCRATCH_OFFSET + i], (unsigned char)(i ^ 0x3C));
    }

    /* 9: the two ways of writing the same field leave the same sector.  This
     * is what licenses replacing a per-byte freeze_poke loop with one fetch,
     * a patch and one store: not that the bytes read back, but that nothing
     * else in the sector moved. */
    for (uint16_t i = 0; i < 512; i++) {
        snapshot[i] = sector_buffer[i];
    }
    /* Scribble, so an unchanged sector cannot pass by doing nothing. */
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        freeze_poke(SCRATCH + i, 0x5A);
    }
    check(9, freeze_fetch_sector(SCRATCH_BASE, NULL), FreezerOk);
    for (uint8_t i = 0; i < SCRATCH_BYTES; i++) {
        sector_buffer[SCRATCH_OFFSET + i] = (unsigned char)(i ^ 0x3C);
    }
    check(9, freeze_store_sector(SCRATCH_BASE, NULL), FreezerOk);
    check(9, freeze_fetch_sector(SCRATCH_BASE, NULL), FreezerOk);
    for (uint16_t i = 0; i < 512; i++) {
        check(9, sector_buffer[i], snapshot[i]);
    }

    debug_msg("PASS");
    finish(0);
}
