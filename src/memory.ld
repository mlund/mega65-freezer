/* Memory this project reserves for itself, outside what the linker allocates.
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

/* Above the region the linker allocates from, which stops at $9000 so that a
 * tool cannot grow into the character ROM shadow.  The shadow is what the VIC
 * reads there -- never the RAM beneath it -- and $A000-$B7FF is RAM too under
 * the $01 = $36 every tool sets, so this whole span is free for a buffer the
 * VIC is not going to look at.  Being under 64KB, it stays reachable with an
 * ordinary pointer, which is what lets both of these be plain arrays.
 *
 * Not in .bss, because .bss shares the $0801-$9000 region with the code: a
 * byte there is a byte the C cannot use, even though it never reaches the .M65
 * file. */
thumbnail_buffer      = 0x9000;
thumbnail_buffer_size = 0x1000;
/* Read back into after a write, to check the card took it.  Every tool links
 * the SD code, so this is 512 bytes off all seven budgets. */
sd_verify_buffer      = 0xA000;
sd_verify_buffer_size = 0x0200;
