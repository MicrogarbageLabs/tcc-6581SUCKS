/*
 *  emu816.h -- a small 65816 interpreter for testing what 816-tcc makes.
 *
 *  Every opcode, native and emulation mode, decimal mode, MVN/MVP, with a
 *  cycle count (CPU cycles, not master clocks: no memory speeds). No
 *  interrupts, no PPU: memory is whatever the host's read/write callbacks
 *  make it.
 *
 *  WDM ($42 nn), a 2-byte no-op on a real 65816, calls the host's wdm
 *  callback with nn: that is how a test talks to the test runner.
 *
 *  Public domain (CC0). No warranty.
 */
#ifndef EMU816_H
#define EMU816_H

#include <stdint.h>

#define EMU_C 0x01
#define EMU_Z 0x02
#define EMU_I 0x04
#define EMU_D 0x08
#define EMU_X 0x10
#define EMU_M 0x20
#define EMU_V 0x40
#define EMU_N 0x80

typedef struct emu816 emu816;

struct emu816 {
    uint16_t a, x, y, s, d, pc;
    uint8_t dbr, pbr, p;
    uint8_t e;                  /* 1 = emulation mode */
    uint8_t stopped;            /* STP, WAI, or the host stopped it */
    uint64_t cycles;

    /* the host: memory, and WDM */
    void *user;
    uint8_t (*read)(emu816 *c, uint32_t addr);
    void (*write)(emu816 *c, uint32_t addr, uint8_t v);
    void (*wdm)(emu816 *c, uint8_t n);
};

/* RESET: emulation mode, the vector at $00:FFFC */
void emu816_reset(emu816 *c);
/* one instruction; returns its cycles */
int emu816_step(emu816 *c);

#endif
