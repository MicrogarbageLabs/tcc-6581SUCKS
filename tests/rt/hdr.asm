; ============================================================
;  hdr.asm — the ROM header and vectors for a compiler test.
;
;  816-tcc puts `.include "hdr.asm"` at the top of every C file's
;  assembly, so the test's C object is the one that carries the header
;  and vectors; crt0 and test.asm include memmap.asm.
;
;  Public domain (CC0). No warranty.
; ============================================================

.include "memmap.asm"

.SNESHEADER
  ID "SNES"

  ;    "123456789012345678901"  ; exactly 21 bytes
  NAME "816-TCC TEST         "

  HIROM
  FASTROM

  CARTRIDGETYPE $00
  ROMSIZE $07
  SRAMSIZE $00
  COUNTRY $01
  LICENSEECODE $00
  VERSION $00
.ENDSNES

.SNESNATIVEVECTOR
  COP t_crash
  BRK t_crash
  ABORT t_crash
  NMI t_crash
  IRQ t_crash
.ENDNATIVEVECTOR

.SNESEMUVECTOR
  COP t_crash
  ABORT t_crash
  NMI t_crash
  RESET t_start
  IRQBRK t_crash
.ENDEMUVECTOR

.BASE $C0
