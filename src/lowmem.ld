/* Low memory the seven tools share.
 *
 * Only what must outlive the program holding it: mega65_dos_exechelper() loads
 * the next tool over this one, so a value handed across that boundary cannot
 * live in either image.  The tools load at $07FF and run upwards, so these sit
 * below that.  Anything a tool merely needs at a particular alignment is an
 * ordinary aligned object and the compiler places it.  Hyppo's
 * freeze_mem_list saves all 384KB of RAM from $0000000 to the freeze slot
 * before the freezer starts and restores it on resume (mega65-core,
 * src/hyppo/freeze.asm), so the frozen program's own bytes here are safe from
 * us and ours are discarded when it resumes.
 *
 * What is not automatic is the tools agreeing with each other: each links
 * separately and sees only its own reservation, so an overlap between two of
 * them is invisible to both.  The whole map therefore lives here, in the one
 * file every target includes -- the on-target tests link the platform's own
 * script and still share this memory -- and link.ld asserts that no two
 * reservations meet.
 */
tool_density      = 0x033C;   /* disk chooser -> MAKEDISK: 0 = DD, 1 = HD  */
tool_density_size = 0x0001;
tool_drive_id      = 0x033D;  /* disk chooser -> MAKEDISK: which drive     */
tool_drive_id_size = 0x0001;
scheme_handoff      = 0x033E; /* 2 bytes, magic then scheme index          */
scheme_handoff_size = 0x0002;
/* The code that performs the load, which cannot live in the image being
 * overwritten. */
loader_stub      = 0x0340;
loader_stub_size = 0x0040;

/* Hyppo copies a page at a time, into a page the caller names by its high byte
 * alone: the trap zeroes the low byte itself and refuses anything above $7EFF
 * (mega65-core, src/hyppo/main.asm:2366).  So the only choice is which page
 * below 32KB, and this one is free because it is the C64's screen -- ours is
 * at $B800.
 *
 * Three structures share it, one at a time: the process descriptor and the
 * freeze region list, 256 bytes each, and the filename hyppo's file traps
 * read.  Each is consumed or copied out before the next is asked for. */
hyppo_page      = 0x0400;
hyppo_page_size = 0x0100;
/* exec loads the next tool over this image, so its filename cannot share a
 * page with an attach name that has still to be used.  The stack page serves:
 * it grows down from $01FF and no tool pushes deep enough to reach a name at
 * the bottom of it. */
hyppo_exec_page      = 0x0100;
hyppo_exec_page_size = 0x0100;
