;;; MEGA65 hypervisor and hardware constants shared by the assembly sources.

;;; Trap registers.  The value written selects the function; the two registers
;;; reach different trap handlers.
	.equ HTRAP_DOS,			$D640
	.equ HTRAP_SYSPART,		$D642

;;; HTRAP_DOS functions.
	.equ HYPPO_GETVERSION,		$00
	.equ HYPPO_GETCURRENTDRIVE,	$04
	.equ HYPPO_CHDIR,		$0C
	.equ HYPPO_OPENDIR,		$12
	.equ HYPPO_READDIR,		$14
	.equ HYPPO_CLOSEDIR,		$16
	.equ HYPPO_CLOSEALL,		$22
	.equ HYPPO_SETNAME,		$2E
	.equ HYPPO_FINDFILE,		$34
	.equ HYPPO_LOADFILE,		$36
	.equ HYPPO_CDROOTDIR,		$3C
	.equ HYPPO_GETPROCDESC,		$48
	.equ HYPPO_ATTACH,		$4A

;;; HTRAP_SYSPART functions.
	.equ SYSPART_SLOT_SECTOR,	$10
	.equ SYSPART_UNFREEZE,		$12
	.equ SYSPART_REGION_LIST,	$14
	.equ SYSPART_SLOT_COUNT,	$16
	.equ SYSPART_GETERRORCODE,	$38

;;; Hyppo needs its filename buffer page-aligned and in the bottom 32KB.  Two
;;; are used so a pending attach name is not clobbered by an exec.
	.equ NAME_BUF_EXEC,		$0100
	.equ NAME_BUF_DOS,		$0400

;;; Scratch page for the loader stub, which must survive the load it performs.
	.equ LOADER_STUB,		$0340
	.equ LOADER_STUB_SIZE,		$80

;;; PRG conventions: files load at $07FF and are entered via the BASIC stub.
	.equ LOAD_ADDR_LO,		$FF
	.equ LOAD_ADDR_HI,		$07
	.equ PROGRAM_ENTRY,		$080D

	.equ NMI_VECTOR,		$0318
	.equ BORDER_COLOUR,		$D020

;;; Drive selection bits for attach/detach.
	.equ DRIVE_MASK,		$01
	.equ DETACH_DRIVE_MASK,		$41	; drive bits plus bit 6 (nodrive)
	.equ DETACH_FLAG,		$80
	.equ ATTACH_LEGACY_BASE,	$40	; pre-1.2 hyppo: $40 or $46
	.equ ATTACH_LEGACY_DRIVE1,	$06

;;; Offsets into hyppo's dirent, written to NAME_BUF_DOS.
	.equ HDIRENT_NAME,		0
	.equ HDIRENT_NAME_LEN,		64	; name is 64 bytes
	.equ HDIRENT_CLUSTER,		77	; 64 + 1 + 12
	.equ HDIRENT_SIZE,		81	; HDIRENT_CLUSTER + 4
	.equ HDIRENT_ATTR,		86	; HDIRENT_SIZE + 1 + 4

;;; Offsets into the C struct m65_dirent.
	.equ DIRENT_INO,		0	; d_ino,    4 bytes
	.equ DIRENT_OFF,		4	; d_off,    2 bytes
	.equ DIRENT_RECLEN,		6	; d_reclen, 4 bytes
	.equ DIRENT_TYPE,		10	; d_type,   2 bytes
	.equ DIRENT_NAME,		12	; d_name
	.equ DIRENT_NAME_MAX,		256

;;; The filename pointer is stashed alongside each buffer for post-mortem
;;; inspection from the monitor.
	.equ NAME_PTR_STASH_EXEC,	NAME_BUF_EXEC + $40
	.equ NAME_PTR_STASH_DOS,	NAME_BUF_DOS + $40

;;; dos_attach gained its current calling convention in hyppo DOS 1.3.
	.equ HDOS_MAJOR_MIN,		1
	.equ HDOS_MINOR_MIN,		3

;;; Border flashes while retrying a failed load, before falling back.
	.equ RETRY_FLASH_COUNT,		60

	.equ ATTACH_ERROR,		$EF
