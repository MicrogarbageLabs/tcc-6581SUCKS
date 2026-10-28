; ============================================================
;  crt0.asm — start-up for a compiler test under run816.
;
;  Derived from PVSnesLib's crt0_snes.asm (Copyright (C) 2012-2024
;  Alekmaul, DigiDwrf; zlib licence, notice below), by way of the HX-422
;  runtime's: no PPU set-up, no consoleInit, no NMI. What the compiler
;  needs stays: the tcc register page at direct page $0000, D = $0000,
;  DB = $7E, the .data copy, the .bss clear and the memcpy/memmove stubs.
;  When main returns, t_done reports the checks.
;
;  ---------------------------------------------------------------
;  Copyright (C) 2012-2024 Alekmaul, DigiDwrf
;
;  This software is provided 'as-is', without any express or implied
;  warranty.  In no event will the authors be held liable for any
;  damages arising from the use of this software.
;
;  Permission is granted to anyone to use this software for any
;  purpose, including commercial applications, and to alter it and
;  redistribute it freely, subject to the following restrictions:
;
;  1. The origin of this software must not be misrepresented; you
;     must not claim that you wrote the original software. If you use
;     this software in a product, an acknowledgment in the product
;     documentation would be appreciated but is not required.
;  2. Altered source versions must be plainly marked as such, and must
;     not be misrepresented as being the original software.
;  3. This notice may not be removed or altered from any source
;     distribution.
; ============================================================

.include "memmap.asm"

.RAMSECTION ".registers" BANK 0 SLOT 1 ORGA 0 FORCE PRIORITY 1000
tcc__registers dsb 0
tcc__r0 dsb 2
tcc__r0h dsb 2
tcc__r1 dsb 2
tcc__r1h dsb 2
tcc__r2 dsb 2
tcc__r2h dsb 2
tcc__r3 dsb 2
tcc__r3h dsb 2
tcc__r4 dsb 2
tcc__r4h dsb 2
tcc__r5 dsb 2
tcc__r5h dsb 2
tcc__r9 dsb 2
tcc__r9h dsb 2
tcc__r10 dsb 2
tcc__r10h dsb 2
tcc__f2 dsb 2
tcc__f2h dsb 2
tcc__f3 dsb 2
tcc__f3h dsb 2
move_insn dsb 4                         ; 3 bytes mvn + 1 byte rts
move_backwards_insn dsb 4               ; 3 bytes mvp + 1 byte rts
tcc__a0 dsb 2                           ; fastcall / regparm parameter slots (816-tcc fork)
tcc__a1 dsb 2
tcc__a2 dsb 2
tcc__a3 dsb 2
.ENDS

; Initialised globals live in ROM ("glob.data") and are copied to WRAM
; ("globram.data") at start-up. Both sections must exist even when empty.
.RAMSECTION "globram.data" BANK $7f SLOT 3 KEEP
.ENDS

.BANK 0
.BASE $C0

.SECTION "glob.data" SEMIFREE ORG ORG_0 KEEP
.ENDS

.EMPTYFILL $00                          ; unused space is BRK: run816 stops on it

.SECTION "t_crt0" SEMIFREE ORG ORG_0

.accu 16
.index 16
.16bit

t_start:
        sei
        clc
        xce                             ; native mode
        jml t_fast                      ; into bank $C0

t_fast:
        rep #$30                        ; binary, A/X/Y 16 bit
        ldx #$1FFF
        txs                             ; stack at $1FFF

        lda.w #tcc__registers
        tad                             ; direct page = the tcc register page

        ; copy initialised globals from ROM to WRAM
        ldx #0
-       lda.l SECTIONSTART_glob.data,x
        sta.l SECTIONSTART_globram.data,x
        inx
        inx
        cpx #(SECTIONEND_glob.data-SECTIONSTART_glob.data)
        bcc -

        ; clear .bss: all of $7E:2000-$FFFF (each C object has its own .bss
        ; section, so the SECTIONSTART_/SECTIONEND_ names cover only one)
        pea $7e7e
        plb
        plb                             ; data bank = $7E, as C expects
        ldx #$E000
-       dex
        dex
        stz.w $2000, x
        bne -

        ; the stubs memcpy and memmove call
        lda #$0054                      ; mvn + first byte
        sta.b move_insn
        lda #$6000                      ; second byte + rts
        sta.b move_insn + 2
        lda #$0044                      ; mvp + first byte
        sta.b move_backwards_insn
        lda #$6000
        sta.b move_backwards_insn + 2

        stz.b tcc__r0
        stz.b tcc__r1

        jsl main
        jsl t_done                      ; never returns

.ENDS
