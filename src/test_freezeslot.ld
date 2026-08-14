/* The on-target test links the platform's own script, which knows nothing of
 * the sections link.ld places.  Without a rule lld treats .sdverify as an
 * orphan and puts it at $0801, over the PRG's own BASIC stub.
 *
 * Ordered after .bss rather than given an address: an address outside the
 * platform's region only makes lld try to fit it inside one.  Where it lands
 * does not matter here -- a test PRG has no $9000 ceiling to respect and no
 * budget to protect -- so long as it is last.
 */
SECTIONS {
    .sdverify (NOLOAD) : { *(.sdverify) }
    .netbuf (NOLOAD) : { *(.netbuf) }
} INSERT AFTER .bss
