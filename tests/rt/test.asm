; ============================================================
;  test.asm — what a compiler test calls to talk to run816.
;
;  Each hook is a WDM ($42 nn), a 2-byte no-op on a real 65816 that run816
;  traps. Values go in A, or in the mailbox at $7F:FF00:
;
;    WDM $01  t_fail(code)      fail now, A = code
;    WDM $02  t_putc(ch)        print a character, A = ch
;    WDM $03  t_check(id, got, want)  mailbox: id, got, want (u16, u32, u32)
;    WDM $04  t_timer_start()   start counting cycles
;    WDM $05  t_timer_stop(id)  print the cycles since the start, A = id
;    WDM $06  t_done()          the end: pass if every check matched
;    WDM $07  t_crash           a BRK, COP or stray interrupt
;
;  C calls them with the arguments on the stack: after the php, the first
;  is at 5,s.
;
;  Public domain (CC0). No warranty.
; ============================================================

.include "memmap.asm"

.DEFINE T_BOX $7FFF00

.BASE $C0
.SECTION "t_hooks" SEMIFREE ORG ORG_0

.accu 16
.index 16

; void t_check(u16 id, u32 got, u32 want)
;   after php: 5,s id   7,s got low  9,s got high  11,s want low  13,s want high
t_check:
        php
        rep #$30
        lda 5,s
        sta.l T_BOX
        lda 7,s
        sta.l T_BOX+2
        lda 9,s
        sta.l T_BOX+4
        lda 11,s
        sta.l T_BOX+6
        lda 13,s
        sta.l T_BOX+8
        .db $42, $03
        plp
        rtl

; void t_putc(u16 ch)
t_putc:
        php
        rep #$30
        lda 5,s
        .db $42, $02
        plp
        rtl

; void t_puts(const char *s)   after php: 5,s low word  7,s bank
t_puts:
        php
        rep #$30
        lda 5,s
        sta.b tcc__r9
        lda 7,s
        sta.b tcc__r9h
        ldy #0
-       lda [tcc__r9],y
        and #$00FF
        beq +
        .db $42, $02
        iny
        bra -
+       plp
        rtl

; void t_timer_start(void)
t_timer_start:
        .db $42, $04
        rtl

; void t_timer_stop(u16 id)
t_timer_stop:
        php
        rep #$30
        lda 5,s
        .db $42, $05
        plp
        rtl

; void t_fail(u16 code)
t_fail:
        rep #$30
        lda 4,s
        .db $42, $01
        stp

; void t_done(void)
t_done:
        .db $42, $06
        stp

t_crash:
        .db $42, $07
        stp

.ENDS
