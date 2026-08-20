/* The low-memory map, shared by the tools and by the on-target tests: both
 * link this file, so both agree on it.  Buffers above $9000 are not here --
 * they are sections, placed in link.ld.
 *
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
 * Four structures share it, one at a time: the process descriptor, the freeze
 * region list, the filename hyppo's file traps read, and the directory entry
 * readdir writes back.  Each is consumed or copied out before the next is
 * asked for. */
hyppo_page      = 0x0400;
hyppo_page_size = 0x0100;
/* The SDK's mega65_h_setname() stages the name in the page this names, so it
 * writes where our own copies went rather than into a page of its own.  Its
 * platform script defaults this to the stack page, which here is the exec
 * name's (below) and must not be shared. */
__mega65_h_name_buf = hyppo_page;
/* exec loads the next tool over this image, so its filename cannot share a
 * page with an attach name that has still to be used.  The stack page serves:
 * it grows down from $01FF and no tool pushes deep enough to reach a name at
 * the bottom of it. */
hyppo_exec_page      = 0x0100;
hyppo_exec_page_size = 0x0100;

ASSERT(tool_density + tool_density_size <= tool_drive_id,
    "low memory: the density byte runs into the drive id")
ASSERT(tool_drive_id + tool_drive_id_size <= scheme_handoff,
    "low memory: the drive id runs into the scheme handoff")
ASSERT(scheme_handoff + scheme_handoff_size <= loader_stub,
    "low memory: the scheme handoff runs into the loader stub")
ASSERT(loader_stub + loader_stub_size <= hyppo_page,
    "low memory: the loader stub runs into hyppo's page")
ASSERT(hyppo_exec_page + hyppo_exec_page_size <= tool_density,
    "low memory: the exec name page runs into the tool handoff bytes")
/* The trap's own rule, so that moving either page fails the build rather than
 * returning dos_errorcode_invalid_address at run time. */
ASSERT((hyppo_page & 0xFF) == 0 && hyppo_page < 0x7F00,
    "hyppo copies only into a page-aligned buffer below $7F00")
ASSERT((hyppo_exec_page & 0xFF) == 0 && hyppo_exec_page < 0x7F00,
    "hyppo copies only into a page-aligned buffer below $7F00")
/* helper.S copies the routine into the reservation, so the routine must fit. */
ASSERT(loadfile_routine_end - loadfile_routine <= loader_stub_size,
    "the loader stub routine outgrew its reservation in low memory")
