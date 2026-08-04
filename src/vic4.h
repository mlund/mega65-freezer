#ifndef __VIC4_H__
#define __VIC4_H__

/* A lock over the VIC-IV extended registers.
 *
 * The extended set -- screen base ($D060), charset base ($D068), colour RAM
 * ($D064), sprite pointers ($D06C-$D06E), row width ($D058-$D059), border
 * positions ($D05C-$D05D) -- has a second writer.  Any store to $D011,
 * $D016, $D018, $D031 or $DD00 recalculates all of it from the legacy VIC-II
 * bits, even when the value stored is unchanged.  Bit 7 of $D05D gates that
 * recalculation, so it works as a mutex: hold it across direct VIC-IV
 * programming and the hot registers cannot interleave.
 *
 * Release cancels a pending update before re-enabling.  A plain read-modify-
 * write (PEEK($D05D) | $80, as mega65-libc's sethotregs(1) does) re-enables
 * with the update still queued, firing all five hot registers at once over
 * whatever was just programmed.
 *
 * Acquiring also suppresses the propagation that hot registers are sometimes
 * used *for*: with the lock held, storing to $D018 no longer moves the
 * screen.  Writes wanted for that side effect belong outside the lock.
 */

#define VIC4_LOCK_ACQUIRE()                                                                        \
    __asm__ volatile("lda #$80\n\t"                                                                \
                     "trb $d05d" ::: "a", "p", "memory")

#define VIC4_LOCK_RELEASE()                                                                        \
    __asm__ volatile("lda #$80\n\t"                                                                \
                     "trb $d05d\n\t"                                                               \
                     "tsb $d05d" ::: "a", "p", "memory")

/* Scoped form: VIC4_LOCKED { ...direct VIC-IV writes... } */
#define VIC4_LOCKED                                                                                \
    for (unsigned char _vic4_held = (VIC4_LOCK_ACQUIRE(), 1); _vic4_held;                          \
        _vic4_held = (VIC4_LOCK_RELEASE(), 0))

#endif /* __VIC4_H__ */
