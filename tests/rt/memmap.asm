; ============================================================
;  memmap.asm — the ROM layout every test object is assembled against.
;
;  Split out from the header on purpose, the way PVSnesLib splits its
;  library header from a project's: EVERY object needs the memory map,
;  but exactly ONE object may carry the .SNESHEADER and the vectors, or
;  they would be defined twice at link time. Here that one object is the
;  C file's, because the compiler emits `.include "hdr.asm"` at the top
;  of its assembly output.
;
;  HiROM, 64 KB banks. run816 maps $C0-$FF to 4 MB of "PSRAM", so
;  bank 0 of the image is $C0:0000-$FFFF and its upper half is what the
;  $00-$3F mirror serves. Code and interrupt handlers therefore live at
;  $8000 and up, which is why every section here uses ORG $8000.
;
;  Public domain (CC0). No warranty.
; ============================================================

; Every object must record the same architecture or wlalink refuses the
; link ("object files are compiled for different memory architectures").
; PVSnesLib's libtcc.obj is built HiROM + FastROM:
;   - .FASTROM is the speed directive its build emits (there is no .HIROM
;     directive: HiROM comes from the map below and the header's keyword)
;   - the slot list has to match the library's exactly, SRAM slot included,
;     even though nothing here uses it
.FASTROM

.MEMORYMAP
  SLOTSIZE $10000
  DEFAULTSLOT 0
  SLOT 0 $0000                  ; ROM bank: HiROM, $C0:0000-$FFFF
  SLOT 1 $0 $2000               ; low RAM, $00:0000-$1FFF
  SLOT 2 $2000 $E000            ; WRAM,    $7E:2000-$FFFF
  SLOT 3 $0 $10000              ; WRAM,    $7F:0000-$FFFF
  SLOT 4 $6000                  ; SRAM window, as in PVSnesLib's map
.ENDME

.ROMBANKSIZE $10000             ; HiROM: 64 KB banks
.ROMBANKS 2                     ; 128 KB

.DEFINE ORG_0 $8000             ; where code sections start inside a bank
