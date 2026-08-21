;;; Flat 28-bit load and store, replacing mega65-libc's pair of the same name.
;;;
;;; Same instructions; what differs is imag.inc, whose .zeropage declarations
;;; get the two-byte base-page form.  Nothing relaxes absolute to base page at
;;; link time, so a file that omits it pays a byte on every register access.
;;;
;;; Both must be defined here.  libc keeps them in one archive member, so
;;; defining both leaves it unpulled; dropping either one pulls the member back
;;; for it and the other then clashes.
;;;
;;; The gathering is unavoidable: the ABI hands a 32-bit argument over as A, X,
;;; __rc2, __rc3, while [zp],z wants four consecutive base-page bytes.
;;;
;;; ldz is belt and braces -- nothing reaches these with Z set, but two bytes
;;; each beats a wrong address on every access if that stops being true.

	.include "imag.inc"

;;; uint8_t lpeek(uint32_t address)
	.section .text.lpeek,"ax",@progbits
	.global lpeek
lpeek:
	sta __rc4
	stx __rc5
	lda __rc2
	sta __rc6
	lda __rc3
	sta __rc7
	ldz #$00
	lda [__rc4], z
	rts

;;; void lpoke(uint32_t address, uint8_t value)
	.section .text.lpoke,"ax",@progbits
	.global lpoke
lpoke:
	sta __rc5
	stx __rc6
	lda __rc2
	sta __rc7
	lda __rc3
	sta __rc8
	lda __rc4			; the value
	ldz #$00
	sta [__rc5], z
	rts
