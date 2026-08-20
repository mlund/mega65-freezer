;;; The one hypervisor call that cannot be C: the exec path, which loads the
;;; next tool over the image it is running from, so the loader has to sit
;;; outside that image.  Everything else lives in hyppo.c.
;;;
;;; Arguments follow the llvm-mos ABI: pointers in __rs1 (__rc2/__rc3) onwards,
;;; scalars in A, X, then __rc4 onwards; char returns in A, unsigned short in
;;; A(lo)/X(hi), pointers in __rc2/__rc3.
;;;
;;; Every return path must leave Z at 0.  The 45GS02 encodes base-page indirect
;;; as (zp),Z, so a stray Z silently offsets every compiler-generated pointer
;;; dereference, with no diagnostic and no fault at the offending site.

#include "helper.h"

	.zeropage __rc2, __rc3, __rc4, __rc5

	.section .zp.bss.hptr,"az",@nobits
;; Indirect addressing needs a zero-page pointer, and the argument registers do
;; not survive the fallback path in mega65_dos_exechelper.
hptr:	.zero 2

;;; void init_nmi(void)
	.section .text.init_nmi,"ax",@progbits
	.global init_nmi
init_nmi:
	sei
	lda #mos16lo(nmi_handler)
	sta NMI_VECTOR
	lda #mos16hi(nmi_handler)
	sta NMI_VECTOR+1
	rts

nmi_handler:
	rti


;;; char mega65_dos_exechelper(char *image_name)
;;; image_name in __rc2/__rc3.  Effectively exec(); only returns on error.
	.section .text.mega65_dos_exechelper,"ax",@progbits
	;; fall-back to FREEZER.M65 if the requested .M65 cannot be loaded
freezer_m65:
	.asciz "freezer.m65"

	.global mega65_dos_exechelper
mega65_dos_exechelper:
	lda __rc2
	sta hptr
	sta NAME_PTR_STASH_EXEC
	lda __rc3
	sta hptr+1
	sta NAME_PTR_STASH_EXEC+1

.Lex_namecopy:
	ldy #0
.Lex_nameloop:
	lda (hptr),y
	sta hyppo_exec_page,y
	iny
	cmp #0
	bne .Lex_nameloop

	;; Name the page just filled in, and close the files hyppo leaks.  Both
	;; are SDK wrappers, called with the ordinary C ABI: the page in A.  They
	;; clobber the argument registers and __rc2 upwards, which is why the name
	;; pointer lives in hptr rather than where it arrived.
	lda #mos16hi(hyppo_exec_page)
	jsr mega65_h_setname_page
	;; XXX Check for error (carry would be clear)

	jsr mega65_h_closeall

	;; Copy the loader stub to LOADER_STUB and run it; it jumps into the loaded
	;; program and does not return on success.
	ldx #$00
.Lex_copy:
	lda loadfile_routine,x
	sta loader_stub,x
	inx
	cpx #(loadfile_routine_end - loadfile_routine)
	bne .Lex_copy

	jsr loader_stub

	;; un-successful? Then try loading freezer instead
	ldz #$00
.Lex_l0:
	ldx #$00
.Lex_l1:
	ldy #$00
.Lex_l2:
	iny
	bne .Lex_l2
	inc BORDER_COLOUR		; colour-cycle the border to indicate missing file
	inx
	bne .Lex_l1
	inz
	cpz #RETRY_FLASH_COUNT
	bne .Lex_l0

	ldz #$00
	lda #mos16lo(freezer_m65)
	sta hptr
	lda #mos16hi(freezer_m65)
	sta hptr+1
	bra .Lex_namecopy

	;; as this is effectively like exec() on unix, it can only return an error
	lda #$01
	ldz #$00
	rts

.global loadfile_routine
loadfile_routine:
	lda #HYPPO_LOADFILE
	ldx #LOAD_ADDR_LO
	ldy #LOAD_ADDR_HI
	ldz #$00
	sta HTRAP_DOS
	nop
	bcs load_succeeded

	rts

load_succeeded:
	ldz #$00
	jmp PROGRAM_ENTRY
.global loadfile_routine_end
loadfile_routine_end:

.Lgp_error:
	lda #$01
	rts






