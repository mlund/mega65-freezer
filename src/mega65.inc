// MEGA65 hypervisor and hardware constants.
//
// Preprocessor defines rather than .equ, so this is one source of truth for
// both helper.S and C.  That is why helper.S is .S: the uppercase extension
// makes the driver run the C preprocessor before assembling.
//
// Do not parenthesise the expressions below.  In 6502 assembly "(expr)" is
// indirect addressing, so "sta (0x0140)" would assemble to something else
// entirely.

// Trap registers.  The value written selects the function; the two registers
// reach different trap handlers.
#define HTRAP_DOS                0xD640
#define HTRAP_SYSPART            0xD642

// HTRAP_DOS functions.
#define HYPPO_GETVERSION         0x00
#define HYPPO_GETCURRENTDRIVE    0x04
#define HYPPO_CHDIR              0x0C
#define HYPPO_OPENDIR            0x12
#define HYPPO_READDIR            0x14
#define HYPPO_CLOSEDIR           0x16
#define HYPPO_CLOSEALL           0x22
#define HYPPO_SETNAME            0x2E
#define HYPPO_FINDFILE           0x34
#define HYPPO_LOADFILE           0x36
#define HYPPO_CDROOTDIR          0x3C
#define HYPPO_GETPROCDESC        0x48
#define HYPPO_ATTACH             0x4A

// HTRAP_SYSPART functions.
#define SYSPART_SLOT_SECTOR      0x10
#define SYSPART_UNFREEZE         0x12
#define SYSPART_REGION_LIST      0x14
#define SYSPART_SLOT_COUNT       0x16
#define SYSPART_GETERRORCODE     0x38

// Hyppo needs its filename buffer page-aligned and in the bottom 32KB.  Two
// are used so a pending attach name is not clobbered by an exec.
#define NAME_BUF_EXEC            0x0100
#define NAME_BUF_DOS             0x0400

// Scratch page for the loader stub, which must survive the load it performs.
#define LOADER_STUB              0x0340
#define LOADER_STUB_SIZE         0x80

// PRG conventions: files load at $07FF and are entered via the BASIC stub.
#define LOAD_ADDR_LO             0xFF
#define LOAD_ADDR_HI             0x07
#define PROGRAM_ENTRY            0x080D

// The VIC-III/IV conceals its registers until this knock is written to
// VICIV.key ($D02F), in this order.
#define VIC4_KNOCK_1             0x47
#define VIC4_KNOCK_2             0x53

#ifndef __ASSEMBLER__
// Registers used only from C, so they are lvalues rather than bare addresses:
// ASCIIKEY reads and assigns directly instead of through PEEK/POKE.  Guarded
// because helper.S includes this file and cannot parse a cast.
#define MMIO8(addr) (*(volatile uint8_t*)(addr))

// F011 floppy controller (45IO27).  See the MEGA65 user guide, "45IO27
// Multi-Function I/O Controller".
#define F011_CONTROL             MMIO8(0xD080)  // motor and drive select
#define F011_COMMAND             MMIO8(0xD081)
#define F011_STATUS              MMIO8(0xD082)  // BUSY is bit 7
#define F011_DATA                MMIO8(0xD087)  // sector byte port
#define F011_CMD_SPINUP          0x20  // guide: write to COMMAND, then poll BUSY
#define F011_STATUS_BUSY         0x80  // guide: bit 7 of STATUS
// The rest are read off this program's own usage, not the user guide, which
// documents only SPINUP and BUSY.  Treat them as less certain.
#define F011_CMD_READ_SECTOR     0x41
#define F011_CMD_STEP            0x10  // steps the head one track
#define F011_CMD_SEEK            0x18  // seeks to the requested track
#define F011_CTRL_MOTOR_LED      0x60  // motor and drive LED on; OR in the drive id
#define F011_STATUS_AT_TRACK0    0x01
#define F011_STATUS_READ_ERROR   0x18  // sector read failed

// SD controller and touch panel.
#define SD_COMMAND               MMIO8(0xD680)
#define SD_STATUS                MMIO8(0xD680)  // same register; reads status
#define SD_STATUS_SDHC           0x10
#define TOUCH_STATUS             MMIO8(0xD6B0)
#define TOUCH_STATUS_EV1_VALID   0x01

// Keyboard event queue: reads the top event as ASCII, 0x00 when empty;
// assigning any value dequeues.  See the user guide, "Keyboard".
#define ASCIIKEY                 MMIO8(0xD610)
#endif // __ASSEMBLER__

#define NMI_VECTOR               0x0318
#define BORDER_COLOUR            0xD020

// Drive selection bits for attach/detach.
#define DRIVE_MASK               0x01
#define DETACH_DRIVE_MASK        0x41  // drive bits plus bit 6 (nodrive)
#define DETACH_FLAG              0x80
#define ATTACH_LEGACY_BASE       0x40  // pre-1.2 hyppo: $40 or $46
#define ATTACH_LEGACY_DRIVE1     0x06

// Offsets into hyppo's dirent, written to NAME_BUF_DOS.
#define HDIRENT_NAME             0
#define HDIRENT_NAME_LEN         64  // name is 64 bytes
#define HDIRENT_CLUSTER          77  // 64 + 1 + 12
#define HDIRENT_SIZE             81  // HDIRENT_CLUSTER + 4
#define HDIRENT_ATTR             86  // HDIRENT_SIZE + 1 + 4

// Offsets into the C struct m65_dirent.
#define DIRENT_INO               0  // d_ino,    4 bytes
#define DIRENT_OFF               4  // d_off,    2 bytes
#define DIRENT_RECLEN            6  // d_reclen, 4 bytes
#define DIRENT_TYPE              10  // d_type,   2 bytes
#define DIRENT_NAME              12  // d_name
#define DIRENT_NAME_MAX          256

// The filename pointer is stashed alongside each buffer for post-mortem
// inspection from the monitor.
#define NAME_PTR_STASH_EXEC      NAME_BUF_EXEC + 0x40
#define NAME_PTR_STASH_DOS       NAME_BUF_DOS + 0x40

// dos_attach gained its current calling convention in hyppo DOS 1.3.
#define HDOS_MAJOR_MIN           1
#define HDOS_MINOR_MIN           3

// Border flashes while retrying a failed load, before falling back.
#define RETRY_FLASH_COUNT        60

#define ATTACH_ERROR             0xEF
