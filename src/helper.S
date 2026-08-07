;;; MEGA65 hypervisor helpers.
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

	.section .bss.haddr,"aw",@nobits
;; The filename copy clobbers A/X, which carry two bytes of the load address.
haddr:	.zero 2

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

	.section .data.hdos_new_attach,"aw",@progbits
	.global hdos_new_attach
hdos_new_attach:
	.byte 1

;;; uint8_t mega65_dos_init(void)   -> A
	.section .text.mega65_dos_init,"ax",@progbits
	.global mega65_dos_init
mega65_dos_init:
	;; this checks hyppo dos version for dos_attach
	lda #HYPPO_GETVERSION
	sta HTRAP_DOS
	clv
	cpy #HDOS_MAJOR_MIN
	bcc .Ldi_old
	bne .Ldi_newer
	cpz #HDOS_MINOR_MIN
	bcc .Ldi_old
.Ldi_newer:
	lda #1
	bra .Ldi_store
.Ldi_old:
	lda #0
.Ldi_store:
	sta hdos_new_attach
	ldz #0
	rts


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
	sta NAME_BUF_EXEC,y
	iny
	cmp #0
	bne .Lex_nameloop

	;;  Call dos_setname()
	ldy #mos16hi(NAME_BUF_EXEC)
	ldx #mos16lo(NAME_BUF_EXEC)
	lda #HYPPO_SETNAME		; dos_setname Hypervisor trap
	sta HTRAP_DOS
	clv			; wasted slot required after hyper trap
	;; XXX Check for error (carry would be clear)

	; close all files to work around hyppo file descriptor leak bug
	lda #HYPPO_CLOSEALL
	sta HTRAP_DOS
	nop

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
loadfile_routine_end:

	.section .bss.attachDrive,"aw",@nobits
attachDrive:
	.zero 1

;;; char mega65_dos_attach(char *image_name, uint8_t driveid)
;;; image_name in __rc2/__rc3, driveid in A.
	.section .text.mega65_dos_attach,"ax",@progbits
	.global mega65_dos_attach
mega65_dos_attach:
	and #DRIVE_MASK		; only drive 0 or 1 allowed
	sta attachDrive

	lda __rc2
	sta hptr
	sta NAME_PTR_STASH_DOS
	lda __rc3
	sta hptr+1
	sta NAME_PTR_STASH_DOS+1

	ldy #0
.Lat_nameloop:
	lda (hptr),y
	sta NAME_BUF_DOS,y
	iny
	cmp #0
	bne .Lat_nameloop

	;;  Call dos_setname()
	ldy #mos16hi(NAME_BUF_DOS)
	ldx #mos16lo(NAME_BUF_DOS)
	lda #HYPPO_SETNAME		; dos_setname Hypervisor trap
	sta HTRAP_DOS
	clv			; wasted slot required after hyper trap
	bcc .Lat_error

	lda hdos_new_attach
	bne .Lat_newcall

	;; backwards compatibility
	lda attachDrive
	beq .Lat_drive0
	lda #ATTACH_LEGACY_DRIVE1
.Lat_drive0:
	clc
	adc #ATTACH_LEGACY_BASE
	bra .Lat_hyppo

.Lat_newcall:
	;; Now we call dos_attach
	ldx attachDrive
	lda #HYPPO_ATTACH		; dos_attach Hypervisor trap
.Lat_hyppo:
	sta HTRAP_DOS
	clv
	bcc .Lat_error
	lda #$00
	rts
.Lat_error:
	lda #ATTACH_ERROR
	rts

;;; char mega65_dos_detach(uint8_t driveid_and_flags)   A -> A
	.section .text.mega65_dos_detach,"ax",@progbits
	.global mega65_dos_detach
mega65_dos_detach:
	and #DETACH_DRIVE_MASK		; only drive 0 or 1, also allow bit 6 - nodrive
	ora #DETACH_FLAG		; set bit 7 for detach operation
	tax
	lda #HYPPO_ATTACH		; dos_attach Hypervisor trap (which does it all)
	sta HTRAP_DOS
	clv
	rts

;;; char mega65_dos_chdir(unsigned char *dirname)
;;; dirname in __rc2/__rc3.
	.section .text.mega65_dos_chdir,"ax",@progbits
	.global mega65_dos_chdir
mega65_dos_chdir:
	lda __rc2
	sta hptr
	sta NAME_PTR_STASH_DOS
	lda __rc3
	sta hptr+1
	sta NAME_PTR_STASH_DOS+1

	ldy #0
.Lcd_nameloop:
	lda (hptr),y
	sta NAME_BUF_DOS,y
	iny
	cmp #0
	bne .Lcd_nameloop

	;;  Call dos_setname()
	ldy #mos16hi(NAME_BUF_DOS)
	ldx #mos16lo(NAME_BUF_DOS)
	lda #HYPPO_SETNAME		; dos_setname Hypervisor trap
	sta HTRAP_DOS
	clv			; wasted slot required after hyper trap
	bcc .Lcd_notfound

	;; Find the file
	lda #HYPPO_FINDFILE
	sta HTRAP_DOS
	clv
	bcc .Lcd_notfound

	;; Try to change directory to it
	lda #HYPPO_CHDIR
	sta HTRAP_DOS
	clv

.Lcd_notfound:
	;; return inverted carry flag, so result of 0 = success
	php
	pla
	and #$01
	eor #$01
	rts

;;; char mega65_dos_cdroot(void)   -> A
	.section .text.mega65_dos_cdroot,"ax",@progbits
	.global mega65_dos_cdroot
mega65_dos_cdroot:
	lda #HYPPO_GETCURRENTDRIVE		; hyppo_dos_getcurrentdrive - returns drive in A
	sta HTRAP_DOS
	clv
	bcc .Lcr_error
	tax			; next need drive in X
	lda #HYPPO_CDROOTDIR		; hyppo_dos_cdrootdir - returns errorcode in A
	sta HTRAP_DOS
	clv
	bcc .Lcr_error
	lda #$00
	rts
.Lcr_error:
	lda #$01
	rts

;;; uint8_t mega65_dos_getprocdesc(uint8_t pagemsb)   A -> A
	.section .text.mega65_dos_getprocdesc,"ax",@progbits
	.global mega65_dos_getprocdesc
mega65_dos_getprocdesc:
	tay
	lda #HYPPO_GETPROCDESC
	sta HTRAP_DOS		; call hyppo
	clv
	bcc .Lgp_error
	lda #$00
	rts
.Lgp_error:
	lda #$01
	rts





;;; char read_file_from_sdcard(char *filename, uint32_t load_address)
;;; filename in __rc2/__rc3; load_address b0=A, b1=X, b2=__rc4, b3=__rc5.
;;;
;;; The hypervisor needs the name in a page-aligned buffer in the bottom 32KB,
;;; so it is copied to NAME_BUF_DOS (no screen lives there).
	.section .text.read_file_from_sdcard,"ax",@progbits
	.global read_file_from_sdcard
read_file_from_sdcard:
	;; Stash the two load-address bytes that live in A/X before the name
	;; copy clobbers them; b2/b3 stay put in __rc4/__rc5.
	sta haddr
	stx haddr+1

	lda __rc2
	sta hptr
	lda __rc3
	sta hptr+1

	ldy #0
.Lrf_nameloop:
	lda (hptr),y
	sta NAME_BUF_DOS,y
	iny
	cmp #0
	bne .Lrf_nameloop

	;;  Call dos_setname()
	ldy #mos16hi(NAME_BUF_DOS)
	ldx #mos16lo(NAME_BUF_DOS)
	lda #HYPPO_SETNAME		; dos_setname Hypervisor trap
	sta HTRAP_DOS
	clv			; wasted slot required after hyper trap
	bcc .Lrf_error

	;; Load address goes to the trap as $00ZZYYXX
	lda __rc4
	taz
	ldx haddr
	ldy haddr+1

	;; Ask hypervisor to do the load
	lda #HYPPO_LOADFILE
	sta HTRAP_DOS
	clv

.Lrf_error:
	;; return inverted carry flag, so result of 0 = success
	php
	pla
	and #$01
	eor #$01
	ldz #$00
	rts

