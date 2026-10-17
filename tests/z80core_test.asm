; z80core_test.asm - CPU-core self test for z80run.py
;
; Exercises the instructions the PL/M-80 -> Z80 code subset relies on:
; PUSH/POP AF (flag save/restore), all eight JP conditions, JP (HL),
; CPL, RLCA/RRA, DJNZ, JR, LDIR, EX (SP),HL, LD SP,HL.
;
; Any wrong behaviour jumps to failN, which writes N to 0C400H and then
; executes an IX (DD) prefix so z80run.py aborts loudly ("UNKNOWN
; opcode").  On success it writes 01 to 0C400H and self-loops;
; z80run.py prints "core result: 01".

	org	0H

	di
	ld	sp,0DFF0H

; --- PUSH AF / POP AF round trip (A and flags really saved) ---
	scf			; C = 1
	ld	a,0A5H
	push	af
	pop	de		; de = (a << 8) | f  ->  d=0A5H, e has C set
	ld	a,d
	cp	0A5H
	jp	nz,fail1
	ld	a,e
	and	1
	jp	z,fail2		; C must have survived the push
	push	de
	pop	af		; restore flags
	jp	nc,fail3	; C must be set again after POP AF

; --- JP (HL) ---
	ld	hl,ok1
	jp	(hl)
	db	0DDH		; not reached; would abort if executed
ok1:

; --- JP PE (overflow condition, signed compare) ---
	ld	a,081H
	add	a,080H		; -127 + -128 -> overflow, PV set
	jp	pe,ok2
	jp	fail4
ok2:

; --- JP P (minus flag clear) ---
	ld	a,080H
	dec	a		; 07FH, S clear
	jp	p,ok3
	jp	fail5
ok3:

; --- CPL ---
	ld	a,00FH
	cpl
	cp	0F0H
	jp	nz,fail6

; --- RLCA / RRA ---
	ld	a,081H
	rlca			; a = 003H, C = 1
	jp	nc,fail7
	cp	003H
	jp	nz,fail8
	scf			; re-arm C: cp above cleared it
	rra			; a = 081H, C = 1
	jp	nc,fail9	; test C before cp destroys it
	cp	081H
	jp	nz,fail10

; --- DJNZ ---
	ld	b,003H
	xor	a		; a = 0
dloop:
	inc	a
	djnz	dloop		; three passes
	cp	003H
	jp	nz,fail11

; --- JR ---
	jr	ok4
	db	0DDH
ok4:

; --- LDIR ---
	ld	hl,data
	ld	de,0C100H
	ld	bc,004H
	ldir
	ld	a,(0C103H)
	cp	044H
	jp	nz,fail12

; --- EX (SP),HL ---
	ld	hl,01234H
	push	hl
	ld	hl,05678H
	ex	(sp),hl		; hl = 1234H, (sp) = 5678H
	pop	de		; de = 5678H
	ld	a,d
	cp	056H
	jp	nz,fail13
	ld	a,h
	cp	012H
	jp	nz,fail14
	ld	a,l
	cp	034H
	jp	nz,fail15

; --- LD SP,HL ---
	ld	hl,0DFF0H
	ld	sp,hl

; --- success ---
	ld	a,001H
	ld	(0C400H),a
self:
	jr	self

fail1:
	ld	a,02H
	jp	freport
fail2:
	ld	a,03H
	jp	freport
fail3:
	ld	a,04H
	jp	freport
fail4:
	ld	a,05H
	jp	freport
fail5:
	ld	a,06H
	jp	freport
fail6:
	ld	a,07H
	jp	freport
fail7:
	ld	a,08H
	jp	freport
fail8:
	ld	a,09H
	jp	freport
fail9:
	ld	a,0AH
	jp	freport
fail10:
	ld	a,0BH
	jp	freport
fail11:
	ld	a,0CH
	jp	freport
fail12:
	ld	a,0DH
	jp	freport
fail13:
	ld	a,0EH
	jp	freport
fail14:
	ld	a,0FH
	jp	freport
fail15:
	ld	a,10H
freport:
	ld	(0C400H),a
	db	0DDH		; abort loudly

data:
	db	011H,022H,033H,044H
