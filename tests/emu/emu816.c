/*
 *  emu816.c -- the 65816 core. See emu816.h.
 *
 *  Written from the WDC W65C816S datasheet. Cycle counts are the
 *  datasheet's: a base per opcode and addressing mode, +1 for each extra
 *  byte a 16-bit access moves, +1 when D's low byte isn't 0 (direct page
 *  modes), +1 when an index crosses a page (or the index is 16 bits) on a
 *  read, +1 for a taken branch (+1 more for a page cross in emulation
 *  mode).
 *
 *  Public domain (CC0). No warranty.
 */
#include "emu816.h"

#define M8(c) ((c)->p & EMU_M)
#define X8(c) ((c)->p & EMU_X)

/* ---- memory ---- */

static uint8_t rd(emu816 *c, uint32_t a) { return c->read(c, a & 0xFFFFFF); }
static void wr(emu816 *c, uint32_t a, uint8_t v) { c->write(c, a & 0xFFFFFF, v); }

/* the byte after a, in an address's wrap: 0 = 24-bit linear (data),
   1 = inside bank 0 (direct page, stack) */
static uint32_t next(uint32_t a, int wrap)
{
    if (wrap)
        return (a & 0xFF0000) | ((a + 1) & 0xFFFF);
    return (a + 1) & 0xFFFFFF;
}

static uint16_t rd16(emu816 *c, uint32_t a, int wrap)
{
    return rd(c, a) | (rd(c, next(a, wrap)) << 8);
}

static void wr16(emu816 *c, uint32_t a, int wrap, uint16_t v)
{
    wr(c, a, v & 0xFF);
    wr(c, next(a, wrap), v >> 8);
}

static uint8_t fetch8(emu816 *c)
{
    uint8_t v = rd(c, ((uint32_t) c->pbr << 16) | c->pc);
    c->pc++;
    return v;
}

static uint16_t fetch16(emu816 *c)
{
    uint16_t lo = fetch8(c);
    return lo | (fetch8(c) << 8);
}

static uint32_t fetch24(emu816 *c)
{
    uint32_t lo = fetch16(c);
    return lo | ((uint32_t) fetch8(c) << 16);
}

/* ---- the stack ---- */
/* In emulation mode the 6502 instructions keep S in page 1; the 65816's
   own (PEA PEI PER PHD PLD JSL RTL and JSR/JMP (abs,X)'s kin) may run
   past it, and S goes back to page 1 afterwards. */

static void push8(emu816 *c, uint8_t v)
{
    wr(c, c->s, v);
    c->s--;
    if (c->e)
        c->s = 0x0100 | (c->s & 0xFF);
}

static uint8_t pull8(emu816 *c)
{
    c->s++;
    if (c->e)
        c->s = 0x0100 | (c->s & 0xFF);
    return rd(c, c->s);
}

static void push16(emu816 *c, uint16_t v)
{
    push8(c, v >> 8);
    push8(c, v & 0xFF);
}

static uint16_t pull16(emu816 *c)
{
    uint16_t lo = pull8(c);
    return lo | (pull8(c) << 8);
}

/* the 65816's own: no page-1 wrap until the instruction is done */
static void npush8(emu816 *c, uint8_t v)
{
    wr(c, c->s, v);
    c->s--;
}

static uint8_t npull8(emu816 *c)
{
    c->s++;
    return rd(c, c->s);
}

static void npush16(emu816 *c, uint16_t v)
{
    npush8(c, v >> 8);
    npush8(c, v & 0xFF);
}

static uint16_t npull16(emu816 *c)
{
    uint16_t lo = npull8(c);
    return lo | (npull8(c) << 8);
}

static void nfix(emu816 *c)
{
    if (c->e)
        c->s = 0x0100 | (c->s & 0xFF);
}

/* ---- flags ---- */

static void setp(emu816 *c, uint8_t p)
{
    if (c->e)
        p |= EMU_M | EMU_X;
    c->p = p;
    if (p & EMU_X) {
        c->x &= 0xFF;
        c->y &= 0xFF;
    }
}

static void nz8(emu816 *c, uint8_t v)
{
    c->p = (c->p & ~(EMU_N | EMU_Z)) | (v & 0x80) | (v ? 0 : EMU_Z);
}

static void nz16(emu816 *c, uint16_t v)
{
    c->p = (c->p & ~(EMU_N | EMU_Z)) | ((v >> 8) & 0x80) | (v ? 0 : EMU_Z);
}

static void setf(emu816 *c, uint8_t f, int on)
{
    if (on)
        c->p |= f;
    else
        c->p &= ~f;
}

/* ---- addressing ---- */
/* Each computes an effective address and its wrap, consuming the operand
   bytes and adding the mode's own cycle penalties to *cy. */

typedef struct {
    uint32_t a;
    int wrap;
} ea_t;

static int dl_pen(emu816 *c) { return (c->d & 0xFF) ? 1 : 0; }

/* a direct page address with an index: in emulation mode with DL = 0 it
   wraps inside the page */
static uint32_t dpaddr(emu816 *c, uint8_t off, uint16_t idx)
{
    if (c->e && (c->d & 0xFF) == 0)
        return (c->d & 0xFF00) | ((off + idx) & 0xFF);
    return (c->d + off + idx) & 0xFFFF;
}

/* a pointer in the direct page: emulation mode with DL = 0 wraps it in
   the page too */
static uint16_t dpptr16(emu816 *c, uint32_t p)
{
    uint32_t q;
    if (c->e && (c->d & 0xFF) == 0)
        q = (p & 0xFF00) | ((p + 1) & 0xFF);
    else
        q = (p + 1) & 0xFFFF;
    return rd(c, p) | (rd(c, q) << 8);
}

static uint32_t dpptr24(emu816 *c, uint32_t p)
{
    return rd(c, p) | (rd(c, (p + 1) & 0xFFFF) << 8) | ((uint32_t) rd(c, (p + 2) & 0xFFFF) << 16);
}

static ea_t am_dp(emu816 *c, int *cy)
{
    ea_t e;
    e.a = dpaddr(c, fetch8(c), 0);
    e.wrap = 1;
    *cy += dl_pen(c);
    return e;
}

static ea_t am_dpx(emu816 *c, int *cy)
{
    ea_t e;
    e.a = dpaddr(c, fetch8(c), c->x);
    e.wrap = 1;
    *cy += dl_pen(c);
    return e;
}

static ea_t am_dpy(emu816 *c, int *cy)
{
    ea_t e;
    e.a = dpaddr(c, fetch8(c), c->y);
    e.wrap = 1;
    *cy += dl_pen(c);
    return e;
}

static ea_t am_dpind(emu816 *c, int *cy)
{
    ea_t e;
    uint32_t p = dpaddr(c, fetch8(c), 0);
    *cy += dl_pen(c);
    e.a = ((uint32_t) c->dbr << 16) | dpptr16(c, p);
    e.wrap = 0;
    return e;
}

static ea_t am_dpindx(emu816 *c, int *cy)
{
    ea_t e;
    uint32_t p = dpaddr(c, fetch8(c), c->x);
    *cy += dl_pen(c);
    e.a = ((uint32_t) c->dbr << 16) | dpptr16(c, p);
    e.wrap = 0;
    return e;
}

/* w: a write (no page-cross saving) */
static ea_t am_dpindy(emu816 *c, int *cy, int w)
{
    ea_t e;
    uint32_t p = dpaddr(c, fetch8(c), 0);
    uint16_t ptr;
    *cy += dl_pen(c);
    ptr = dpptr16(c, p);
    if (!w && (!X8(c) || ((ptr & 0xFF00) != ((ptr + c->y) & 0xFF00))))
        *cy += 1;
    e.a = ((((uint32_t) c->dbr << 16) | ptr) + c->y) & 0xFFFFFF;
    e.wrap = 0;
    return e;
}

static ea_t am_dplong(emu816 *c, int *cy)
{
    ea_t e;
    uint32_t p = (c->d + fetch8(c)) & 0xFFFF;
    *cy += dl_pen(c);
    e.a = dpptr24(c, p);
    e.wrap = 0;
    return e;
}

static ea_t am_dplongy(emu816 *c, int *cy)
{
    ea_t e;
    uint32_t p = (c->d + fetch8(c)) & 0xFFFF;
    *cy += dl_pen(c);
    e.a = (dpptr24(c, p) + c->y) & 0xFFFFFF;
    e.wrap = 0;
    return e;
}

static ea_t am_abs(emu816 *c)
{
    ea_t e;
    e.a = ((uint32_t) c->dbr << 16) | fetch16(c);
    e.wrap = 0;
    return e;
}

static ea_t am_absi(emu816 *c, int *cy, uint16_t idx, int w)
{
    ea_t e;
    uint16_t base = fetch16(c);
    if (!w && (!X8(c) || ((base & 0xFF00) != ((base + idx) & 0xFF00))))
        *cy += 1;
    e.a = ((((uint32_t) c->dbr << 16) | base) + idx) & 0xFFFFFF;
    e.wrap = 0;
    return e;
}

static ea_t am_long(emu816 *c)
{
    ea_t e;
    e.a = fetch24(c);
    e.wrap = 0;
    return e;
}

static ea_t am_longx(emu816 *c)
{
    ea_t e;
    e.a = (fetch24(c) + c->x) & 0xFFFFFF;
    e.wrap = 0;
    return e;
}

static ea_t am_sr(emu816 *c)
{
    ea_t e;
    e.a = (c->s + fetch8(c)) & 0xFFFF;
    e.wrap = 1;
    return e;
}

static ea_t am_sry(emu816 *c)
{
    ea_t e;
    uint32_t p = (c->s + fetch8(c)) & 0xFFFF;
    uint16_t ptr = rd16(c, p, 1);
    e.a = ((((uint32_t) c->dbr << 16) | ptr) + c->y) & 0xFFFFFF;
    e.wrap = 0;
    return e;
}

/* ---- the ALU ---- */

static void op_adc(emu816 *c, uint16_t v)
{
    int carry = c->p & EMU_C;
    int r;
    if (M8(c)) {
        uint8_t a = c->a & 0xFF;
        v &= 0xFF;
        if (!(c->p & EMU_D)) {
            r = a + v + carry;
        } else {
            r = (a & 0x0F) + (v & 0x0F) + carry;
            if (r > 0x09)
                r += 0x06;
            carry = r > 0x0F;
            r = (a & 0xF0) + (v & 0xF0) + (carry << 4) + (r & 0x0F);
        }
        setf(c, EMU_V, ~(a ^ v) & (a ^ r) & 0x80);
        if ((c->p & EMU_D) && r > 0x9F)
            r += 0x60;
        setf(c, EMU_C, r > 0xFF);
        c->a = (c->a & 0xFF00) | (r & 0xFF);
        nz8(c, r & 0xFF);
    } else {
        uint16_t a = c->a;
        if (!(c->p & EMU_D)) {
            r = a + v + carry;
        } else {
            r = (a & 0x000F) + (v & 0x000F) + carry;
            if (r > 0x0009)
                r += 0x0006;
            carry = r > 0x000F;
            r = (a & 0x00F0) + (v & 0x00F0) + (carry << 4) + (r & 0x000F);
            if (r > 0x009F)
                r += 0x0060;
            carry = r > 0x00FF;
            r = (a & 0x0F00) + (v & 0x0F00) + (carry << 8) + (r & 0x00FF);
            if (r > 0x09FF)
                r += 0x0600;
            carry = r > 0x0FFF;
            r = (a & 0xF000) + (v & 0xF000) + (carry << 12) + (r & 0x0FFF);
        }
        setf(c, EMU_V, ~(a ^ v) & (a ^ r) & 0x8000);
        if ((c->p & EMU_D) && r > 0x9FFF)
            r += 0x6000;
        setf(c, EMU_C, r > 0xFFFF);
        c->a = r & 0xFFFF;
        nz16(c, c->a);
    }
}

static void op_sbc(emu816 *c, uint16_t v)
{
    int carry = c->p & EMU_C;
    int r;
    if (M8(c)) {
        uint8_t a = c->a & 0xFF;
        v = ~v & 0xFF;
        if (!(c->p & EMU_D)) {
            r = a + v + carry;
        } else {
            r = (a & 0x0F) + (v & 0x0F) + carry;
            if (r <= 0x0F)
                r -= 0x06;
            carry = r > 0x0F;
            r = (a & 0xF0) + (v & 0xF0) + (carry << 4) + (r & 0x0F);
        }
        setf(c, EMU_V, ~(a ^ v) & (a ^ r) & 0x80);
        if ((c->p & EMU_D) && r <= 0xFF)
            r -= 0x60;
        setf(c, EMU_C, r > 0xFF);
        c->a = (c->a & 0xFF00) | (r & 0xFF);
        nz8(c, r & 0xFF);
    } else {
        uint16_t a = c->a;
        v = ~v & 0xFFFF;
        if (!(c->p & EMU_D)) {
            r = a + v + carry;
        } else {
            r = (a & 0x000F) + (v & 0x000F) + carry;
            if (r <= 0x000F)
                r -= 0x0006;
            carry = r > 0x000F;
            r = (a & 0x00F0) + (v & 0x00F0) + (carry << 4) + (r & 0x000F);
            if (r <= 0x00FF)
                r -= 0x0060;
            carry = r > 0x00FF;
            r = (a & 0x0F00) + (v & 0x0F00) + (carry << 8) + (r & 0x00FF);
            if (r <= 0x0FFF)
                r -= 0x0600;
            carry = r > 0x0FFF;
            r = (a & 0xF000) + (v & 0xF000) + (carry << 12) + (r & 0x0FFF);
        }
        setf(c, EMU_V, ~(a ^ v) & (a ^ r) & 0x8000);
        if ((c->p & EMU_D) && r <= 0xFFFF)
            r -= 0x6000;
        setf(c, EMU_C, r > 0xFFFF);
        c->a = r & 0xFFFF;
        nz16(c, c->a);
    }
}

static void op_cmp(emu816 *c, uint16_t reg, uint16_t v, int eight)
{
    if (eight) {
        int r = (reg & 0xFF) - (v & 0xFF);
        setf(c, EMU_C, r >= 0);
        nz8(c, r & 0xFF);
    } else {
        int r = reg - v;
        setf(c, EMU_C, r >= 0);
        nz16(c, r & 0xFFFF);
    }
}

/* the group-1 operation on A with operand v */
static void g1(emu816 *c, int op, uint16_t v)
{
    switch (op) {
    case 0: /* ORA */
        if (M8(c)) {
            c->a = (c->a & 0xFF00) | ((c->a | v) & 0xFF);
            nz8(c, c->a & 0xFF);
        } else {
            c->a |= v;
            nz16(c, c->a);
        }
        break;
    case 1: /* AND */
        if (M8(c)) {
            c->a = (c->a & 0xFF00) | ((c->a & v) & 0xFF);
            nz8(c, c->a & 0xFF);
        } else {
            c->a &= v;
            nz16(c, c->a);
        }
        break;
    case 2: /* EOR */
        if (M8(c)) {
            c->a = (c->a & 0xFF00) | ((c->a ^ v) & 0xFF);
            nz8(c, c->a & 0xFF);
        } else {
            c->a ^= v;
            nz16(c, c->a);
        }
        break;
    case 3:
        op_adc(c, v);
        break;
    case 5: /* LDA */
        if (M8(c)) {
            c->a = (c->a & 0xFF00) | (v & 0xFF);
            nz8(c, v & 0xFF);
        } else {
            c->a = v;
            nz16(c, v);
        }
        break;
    case 6:
        op_cmp(c, c->a, v, M8(c));
        break;
    case 7:
        op_sbc(c, v);
        break;
    }
}

/* read-modify-write operations; returns the new value */
static uint16_t rmw(emu816 *c, int op, uint16_t v, int eight)
{
    uint16_t msb = eight ? 0x80 : 0x8000;
    uint16_t mask = eight ? 0xFF : 0xFFFF;
    uint16_t r = 0;
    switch (op) {
    case 0: /* ASL */
        setf(c, EMU_C, v & msb);
        r = (v << 1) & mask;
        break;
    case 1: /* ROL */
        r = ((v << 1) | (c->p & EMU_C)) & mask;
        setf(c, EMU_C, v & msb);
        break;
    case 2: /* LSR */
        setf(c, EMU_C, v & 1);
        r = v >> 1;
        break;
    case 3: /* ROR */
        r = (v >> 1) | ((c->p & EMU_C) ? msb : 0);
        setf(c, EMU_C, v & 1);
        break;
    case 4: /* INC */
        r = (v + 1) & mask;
        break;
    case 5: /* DEC */
        r = (v - 1) & mask;
        break;
    }
    if (eight)
        nz8(c, r);
    else
        nz16(c, r);
    return r;
}

/* ---- memory operands by width ---- */

static uint16_t ldm(emu816 *c, ea_t e, int *cy)
{
    if (M8(c))
        return rd(c, e.a);
    *cy += 1;
    return rd16(c, e.a, e.wrap);
}

static void stm(emu816 *c, ea_t e, uint16_t v, int *cy)
{
    if (M8(c)) {
        wr(c, e.a, v & 0xFF);
    } else {
        *cy += 1;
        wr16(c, e.a, e.wrap, v);
    }
}

static uint16_t ldx(emu816 *c, ea_t e, int *cy)
{
    if (X8(c))
        return rd(c, e.a);
    *cy += 1;
    return rd16(c, e.a, e.wrap);
}

static void stx(emu816 *c, ea_t e, uint16_t v, int *cy)
{
    if (X8(c)) {
        wr(c, e.a, v & 0xFF);
    } else {
        *cy += 1;
        wr16(c, e.a, e.wrap, v);
    }
}

static void rmw_mem(emu816 *c, int op, ea_t e, int *cy)
{
    if (M8(c)) {
        wr(c, e.a, rmw(c, op, rd(c, e.a), 1));
    } else {
        uint16_t v = rd16(c, e.a, e.wrap);
        *cy += 2;
        v = rmw(c, op, v, 0);
        wr(c, next(e.a, e.wrap), v >> 8);
        wr(c, e.a, v & 0xFF);
    }
}

static void tsb_trb(emu816 *c, ea_t e, int set, int *cy)
{
    if (M8(c)) {
        uint8_t v = rd(c, e.a), a = c->a & 0xFF;
        setf(c, EMU_Z, (v & a) == 0);
        wr(c, e.a, set ? (v | a) : (v & ~a));
    } else {
        uint16_t v = rd16(c, e.a, e.wrap);
        *cy += 2;
        setf(c, EMU_Z, (v & c->a) == 0);
        v = set ? (v | c->a) : (v & ~c->a);
        wr(c, next(e.a, e.wrap), v >> 8);
        wr(c, e.a, v & 0xFF);
    }
}

static void bit_op(emu816 *c, uint16_t v, int imm)
{
    if (M8(c)) {
        setf(c, EMU_Z, ((c->a & v) & 0xFF) == 0);
        if (!imm) {
            setf(c, EMU_N, v & 0x80);
            setf(c, EMU_V, v & 0x40);
        }
    } else {
        setf(c, EMU_Z, (c->a & v) == 0);
        if (!imm) {
            setf(c, EMU_N, v & 0x8000);
            setf(c, EMU_V, v & 0x4000);
        }
    }
}

static int branch(emu816 *c, int take)
{
    int8_t off = (int8_t) fetch8(c);
    uint16_t to;
    if (!take)
        return 2;
    to = c->pc + off;
    if (c->e && ((to & 0xFF00) != (c->pc & 0xFF00))) {
        c->pc = to;
        return 4;
    }
    c->pc = to;
    return 3;
}

static void interrupt(emu816 *c, uint16_t nvec, uint16_t evec)
{
    if (c->e) {
        push16(c, c->pc);
        push8(c, c->p | 0x10);  /* B */
        c->pc = rd16(c, evec, 0);
    } else {
        push8(c, c->pbr);
        push16(c, c->pc);
        push8(c, c->p);
        c->pc = rd16(c, nvec, 0);
    }
    c->p = (c->p | EMU_I) & ~EMU_D;
    c->pbr = 0;
}

void emu816_reset(emu816 *c)
{
    c->e = 1;
    c->p = EMU_M | EMU_X | EMU_I;
    c->d = 0;
    c->dbr = 0;
    c->pbr = 0;
    c->s = 0x01FF;
    c->x &= 0xFF;
    c->y &= 0xFF;
    c->stopped = 0;
    c->pc = rd16(c, 0xFFFC, 0);
}

/* the group-1 addressing modes by the opcode's low 5 bits; *cy gets the
   mode's base cycles for an 8-bit access */
static int g1_ea(emu816 *c, int lo5, int w, ea_t *e, int *cy)
{
    switch (lo5) {
    case 0x01: *cy = 6; *e = am_dpindx(c, cy); return 1;
    case 0x03: *cy = 4; *e = am_sr(c); return 1;
    case 0x05: *cy = 3; *e = am_dp(c, cy); return 1;
    case 0x07: *cy = 6; *e = am_dplong(c, cy); return 1;
    case 0x0D: *cy = 4; *e = am_abs(c); return 1;
    case 0x0F: *cy = 5; *e = am_long(c); return 1;
    case 0x11: *cy = w ? 6 : 5; *e = am_dpindy(c, cy, w); return 1;
    case 0x12: *cy = 5; *e = am_dpind(c, cy); return 1;
    case 0x13: *cy = 7; *e = am_sry(c); return 1;
    case 0x15: *cy = 4; *e = am_dpx(c, cy); return 1;
    case 0x17: *cy = 6; *e = am_dplongy(c, cy); return 1;
    case 0x19: *cy = w ? 5 : 4; *e = am_absi(c, cy, c->y, w); return 1;
    case 0x1D: *cy = w ? 5 : 4; *e = am_absi(c, cy, c->x, w); return 1;
    case 0x1F: *cy = 5; *e = am_longx(c); return 1;
    }
    return 0;
}

int emu816_step(emu816 *c)
{
    uint8_t op;
    int cy = 2, lo5;
    ea_t e;
    uint16_t v;

    if (c->stopped)
        return 0;
    if (c->e) {                 /* what emulation mode holds whatever was there */
        c->s = 0x0100 | (c->s & 0xFF);
        c->p |= EMU_M | EMU_X;
        c->x &= 0xFF;
        c->y &= 0xFF;
    }
    op = fetch8(c);
    lo5 = op & 0x1F;

    /* group 1: ORA AND EOR ADC STA LDA CMP SBC */
    if (lo5 == 0x01 || lo5 == 0x03 || lo5 == 0x05 || lo5 == 0x07 || lo5 == 0x09 || lo5 == 0x0D
        || lo5 == 0x0F || lo5 == 0x11 || lo5 == 0x12 || lo5 == 0x13 || lo5 == 0x15 || lo5 == 0x17
        || lo5 == 0x19 || lo5 == 0x1D || lo5 == 0x1F) {
        int g = op >> 5;
        if (lo5 == 0x09) {      /* immediate */
            if (g == 4) {       /* $89: BIT # */
                v = M8(c) ? fetch8(c) : fetch16(c);
                cy = M8(c) ? 2 : 3;
                bit_op(c, v, 1);
            } else {
                v = M8(c) ? fetch8(c) : fetch16(c);
                cy = M8(c) ? 2 : 3;
                g1(c, g, v);
            }
        } else {
            g1_ea(c, lo5, g == 4, &e, &cy);
            if (g == 4)
                stm(c, e, c->a, &cy);
            else
                g1(c, g, ldm(c, e, &cy));
        }
        c->cycles += cy;
        return cy;
    }

    switch (op) {
    /* ---- read-modify-write: ASL ROL LSR ROR on memory and A ---- */
    case 0x06: case 0x26: case 0x46: case 0x66:
        cy = 5; e = am_dp(c, &cy); rmw_mem(c, op >> 5, e, &cy); break;
    case 0x0E: case 0x2E: case 0x4E: case 0x6E:
        cy = 6; e = am_abs(c); rmw_mem(c, op >> 5, e, &cy); break;
    case 0x16: case 0x36: case 0x56: case 0x76:
        cy = 6; e = am_dpx(c, &cy); rmw_mem(c, op >> 5, e, &cy); break;
    case 0x1E: case 0x3E: case 0x5E: case 0x7E:
        cy = 7; e = am_absi(c, &cy, c->x, 1); rmw_mem(c, op >> 5, e, &cy); break;
    case 0x0A: case 0x2A: case 0x4A: case 0x6A:
        if (M8(c))
            c->a = (c->a & 0xFF00) | rmw(c, op >> 5, c->a & 0xFF, 1);
        else
            c->a = rmw(c, op >> 5, c->a, 0);
        break;
    /* INC / DEC */
    case 0xE6: cy = 5; e = am_dp(c, &cy); rmw_mem(c, 4, e, &cy); break;
    case 0xEE: cy = 6; e = am_abs(c); rmw_mem(c, 4, e, &cy); break;
    case 0xF6: cy = 6; e = am_dpx(c, &cy); rmw_mem(c, 4, e, &cy); break;
    case 0xFE: cy = 7; e = am_absi(c, &cy, c->x, 1); rmw_mem(c, 4, e, &cy); break;
    case 0xC6: cy = 5; e = am_dp(c, &cy); rmw_mem(c, 5, e, &cy); break;
    case 0xCE: cy = 6; e = am_abs(c); rmw_mem(c, 5, e, &cy); break;
    case 0xD6: cy = 6; e = am_dpx(c, &cy); rmw_mem(c, 5, e, &cy); break;
    case 0xDE: cy = 7; e = am_absi(c, &cy, c->x, 1); rmw_mem(c, 5, e, &cy); break;
    case 0x1A:
        if (M8(c)) c->a = (c->a & 0xFF00) | rmw(c, 4, c->a & 0xFF, 1);
        else c->a = rmw(c, 4, c->a, 0);
        break;
    case 0x3A:
        if (M8(c)) c->a = (c->a & 0xFF00) | rmw(c, 5, c->a & 0xFF, 1);
        else c->a = rmw(c, 5, c->a, 0);
        break;
    /* TSB / TRB */
    case 0x04: cy = 5; e = am_dp(c, &cy); tsb_trb(c, e, 1, &cy); break;
    case 0x0C: cy = 6; e = am_abs(c); tsb_trb(c, e, 1, &cy); break;
    case 0x14: cy = 5; e = am_dp(c, &cy); tsb_trb(c, e, 0, &cy); break;
    case 0x1C: cy = 6; e = am_abs(c); tsb_trb(c, e, 0, &cy); break;

    /* ---- BIT ---- */
    case 0x24: cy = 3; e = am_dp(c, &cy); bit_op(c, ldm(c, e, &cy), 0); break;
    case 0x2C: cy = 4; e = am_abs(c); bit_op(c, ldm(c, e, &cy), 0); break;
    case 0x34: cy = 4; e = am_dpx(c, &cy); bit_op(c, ldm(c, e, &cy), 0); break;
    case 0x3C: cy = 4; e = am_absi(c, &cy, c->x, 0); bit_op(c, ldm(c, e, &cy), 0); break;

    /* ---- STZ ---- */
    case 0x64: cy = 3; e = am_dp(c, &cy); stm(c, e, 0, &cy); break;
    case 0x74: cy = 4; e = am_dpx(c, &cy); stm(c, e, 0, &cy); break;
    case 0x9C: cy = 4; e = am_abs(c); stm(c, e, 0, &cy); break;
    case 0x9E: cy = 5; e = am_absi(c, &cy, c->x, 1); stm(c, e, 0, &cy); break;

    /* ---- LDX LDY STX STY CPX CPY ---- */
    case 0xA2:
        if (X8(c)) { c->x = fetch8(c); nz8(c, c->x); }
        else { c->x = fetch16(c); nz16(c, c->x); cy = 3; }
        break;
    case 0xA0:
        if (X8(c)) { c->y = fetch8(c); nz8(c, c->y); }
        else { c->y = fetch16(c); nz16(c, c->y); cy = 3; }
        break;
    case 0xA6: cy = 3; e = am_dp(c, &cy); goto do_ldx;
    case 0xB6: cy = 4; e = am_dpy(c, &cy); goto do_ldx;
    case 0xAE: cy = 4; e = am_abs(c); goto do_ldx;
    case 0xBE: cy = 4; e = am_absi(c, &cy, c->y, 0);
    do_ldx:
        c->x = ldx(c, e, &cy);
        if (X8(c)) nz8(c, c->x); else nz16(c, c->x);
        break;
    case 0xA4: cy = 3; e = am_dp(c, &cy); goto do_ldy;
    case 0xB4: cy = 4; e = am_dpx(c, &cy); goto do_ldy;
    case 0xAC: cy = 4; e = am_abs(c); goto do_ldy;
    case 0xBC: cy = 4; e = am_absi(c, &cy, c->x, 0);
    do_ldy:
        c->y = ldx(c, e, &cy);
        if (X8(c)) nz8(c, c->y); else nz16(c, c->y);
        break;
    case 0x86: cy = 3; e = am_dp(c, &cy); stx(c, e, c->x, &cy); break;
    case 0x96: cy = 4; e = am_dpy(c, &cy); stx(c, e, c->x, &cy); break;
    case 0x8E: cy = 4; e = am_abs(c); stx(c, e, c->x, &cy); break;
    case 0x84: cy = 3; e = am_dp(c, &cy); stx(c, e, c->y, &cy); break;
    case 0x94: cy = 4; e = am_dpx(c, &cy); stx(c, e, c->y, &cy); break;
    case 0x8C: cy = 4; e = am_abs(c); stx(c, e, c->y, &cy); break;
    case 0xE0:
        if (X8(c)) op_cmp(c, c->x, fetch8(c), 1);
        else { op_cmp(c, c->x, fetch16(c), 0); cy = 3; }
        break;
    case 0xC0:
        if (X8(c)) op_cmp(c, c->y, fetch8(c), 1);
        else { op_cmp(c, c->y, fetch16(c), 0); cy = 3; }
        break;
    case 0xE4: cy = 3; e = am_dp(c, &cy); op_cmp(c, c->x, ldx(c, e, &cy), X8(c)); break;
    case 0xEC: cy = 4; e = am_abs(c); op_cmp(c, c->x, ldx(c, e, &cy), X8(c)); break;
    case 0xC4: cy = 3; e = am_dp(c, &cy); op_cmp(c, c->y, ldx(c, e, &cy), X8(c)); break;
    case 0xCC: cy = 4; e = am_abs(c); op_cmp(c, c->y, ldx(c, e, &cy), X8(c)); break;

    /* ---- index registers ---- */
    case 0xE8: c->x = X8(c) ? ((c->x + 1) & 0xFF) : (c->x + 1); goto nzx;
    case 0xCA: c->x = X8(c) ? ((c->x - 1) & 0xFF) : (c->x - 1); goto nzx;
    case 0xC8: c->y = X8(c) ? ((c->y + 1) & 0xFF) : (c->y + 1); goto nzy;
    case 0x88: c->y = X8(c) ? ((c->y - 1) & 0xFF) : (c->y - 1); goto nzy;
    nzx:
        if (X8(c)) nz8(c, c->x); else nz16(c, c->x);
        break;
    nzy:
        if (X8(c)) nz8(c, c->y); else nz16(c, c->y);
        break;

    /* ---- transfers ---- */
    case 0xAA: /* TAX */
        c->x = X8(c) ? (c->a & 0xFF) : c->a; goto nzx;
    case 0xA8: /* TAY */
        c->y = X8(c) ? (c->a & 0xFF) : c->a; goto nzy;
    case 0x8A: /* TXA */
        if (M8(c)) { c->a = (c->a & 0xFF00) | (c->x & 0xFF); nz8(c, c->a & 0xFF); }
        else { c->a = c->x; nz16(c, c->a); }
        break;
    case 0x98: /* TYA */
        if (M8(c)) { c->a = (c->a & 0xFF00) | (c->y & 0xFF); nz8(c, c->a & 0xFF); }
        else { c->a = c->y; nz16(c, c->a); }
        break;
    case 0x9B: /* TXY */
        c->y = c->x; goto nzy;
    case 0xBB: /* TYX */
        c->x = c->y; goto nzx;
    case 0xBA: /* TSX */
        c->x = X8(c) ? (c->s & 0xFF) : c->s; goto nzx;
    case 0x9A: /* TXS */
        if (c->e) c->s = 0x0100 | (c->x & 0xFF);
        else c->s = c->x;
        break;
    case 0x5B: /* TCD */
        c->d = c->a; nz16(c, c->d); break;
    case 0x7B: /* TDC */
        c->a = c->d; nz16(c, c->a); break;
    case 0x1B: /* TCS */
        c->s = c->e ? (0x0100 | (c->a & 0xFF)) : c->a; break;
    case 0x3B: /* TSC */
        c->a = c->s; nz16(c, c->a); break;
    case 0xEB: /* XBA */
        c->a = (c->a >> 8) | (c->a << 8);
        nz8(c, c->a & 0xFF);
        cy = 3;
        break;

    /* ---- flags ---- */
    case 0x18: c->p &= ~EMU_C; break;
    case 0x38: c->p |= EMU_C; break;
    case 0x58: c->p &= ~EMU_I; break;
    case 0x78: c->p |= EMU_I; break;
    case 0xB8: c->p &= ~EMU_V; break;
    case 0xD8: c->p &= ~EMU_D; break;
    case 0xF8: c->p |= EMU_D; break;
    case 0xC2: setp(c, c->p & ~fetch8(c)); cy = 3; break;
    case 0xE2: setp(c, c->p | fetch8(c)); cy = 3; break;
    case 0xFB: { /* XCE */
        int carry = c->p & EMU_C;
        setf(c, EMU_C, c->e);
        c->e = carry ? 1 : 0;
        if (c->e) {
            setp(c, c->p | EMU_M | EMU_X);
            c->s = 0x0100 | (c->s & 0xFF);
        }
        break;
    }

    /* ---- the stack ---- */
    case 0x48: cy = 3; if (M8(c)) push8(c, c->a & 0xFF); else { push16(c, c->a); cy++; } break;
    case 0xDA: cy = 3; if (X8(c)) push8(c, c->x & 0xFF); else { push16(c, c->x); cy++; } break;
    case 0x5A: cy = 3; if (X8(c)) push8(c, c->y & 0xFF); else { push16(c, c->y); cy++; } break;
    case 0x08: cy = 3; push8(c, c->p); break;
    case 0x8B: cy = 3; push8(c, c->dbr); break;
    case 0x4B: cy = 3; push8(c, c->pbr); break;
    case 0x0B: cy = 4; npush16(c, c->d); nfix(c); break;
    case 0x68:
        cy = 4;
        if (M8(c)) { c->a = (c->a & 0xFF00) | pull8(c); nz8(c, c->a & 0xFF); }
        else { c->a = pull16(c); nz16(c, c->a); cy++; }
        break;
    case 0xFA:
        cy = 4;
        if (X8(c)) { c->x = pull8(c); nz8(c, c->x); }
        else { c->x = pull16(c); nz16(c, c->x); cy++; }
        break;
    case 0x7A:
        cy = 4;
        if (X8(c)) { c->y = pull8(c); nz8(c, c->y); }
        else { c->y = pull16(c); nz16(c, c->y); cy++; }
        break;
    case 0x28: cy = 4; setp(c, pull8(c)); break;
    case 0xAB: cy = 4; c->dbr = npull8(c); nfix(c); nz8(c, c->dbr); break;
    case 0x2B: cy = 5; c->d = npull16(c); nfix(c); nz16(c, c->d); break;
    case 0xF4: cy = 5; npush16(c, fetch16(c)); nfix(c); break;         /* PEA */
    case 0xD4: { /* PEI */
        uint32_t p;
        cy = 6 + dl_pen(c);
        p = (c->d + fetch8(c)) & 0xFFFF;
        npush16(c, rd16(c, p, 1));
        nfix(c);
        break;
    }
    case 0x62: { /* PER */
        uint16_t off = fetch16(c);
        cy = 6;
        npush16(c, c->pc + off);
        nfix(c);
        break;
    }

    /* ---- jumps and calls ---- */
    case 0x4C: cy = 3; c->pc = fetch16(c); break;
    case 0x5C: { /* JML long */
        uint32_t t = fetch24(c);
        cy = 4;
        c->pc = t & 0xFFFF;
        c->pbr = t >> 16;
        break;
    }
    case 0x6C: { /* JMP (abs): the pointer in bank 0 */
        uint16_t p = fetch16(c);
        cy = 5;
        c->pc = rd16(c, p, 1);
        break;
    }
    case 0x7C: { /* JMP (abs,X): the pointer in the program bank */
        uint16_t p = fetch16(c) + c->x;
        cy = 6;
        c->pc = rd16(c, ((uint32_t) c->pbr << 16) | p, 1);
        break;
    }
    case 0xDC: { /* JML [abs] */
        uint16_t p = fetch16(c);
        cy = 6;
        c->pc = rd16(c, p, 1);
        c->pbr = rd(c, (p + 2) & 0xFFFF);
        break;
    }
    case 0x20: { /* JSR abs */
        uint16_t t = fetch16(c);
        cy = 6;
        push16(c, c->pc - 1);
        c->pc = t;
        break;
    }
    case 0xFC: { /* JSR (abs,X) */
        uint16_t lo = fetch8(c), p;
        cy = 8;
        npush16(c, c->pc);      /* the pc of the high operand byte */
        p = (lo | (fetch8(c) << 8)) + c->x;
        nfix(c);
        c->pc = rd16(c, ((uint32_t) c->pbr << 16) | p, 1);
        break;
    }
    case 0x22: { /* JSL */
        uint16_t lo = fetch16(c);
        cy = 8;
        npush8(c, c->pbr);
        c->pbr = fetch8(c);
        npush16(c, c->pc - 1);
        nfix(c);
        c->pc = lo;
        break;
    }
    case 0x60: cy = 6; c->pc = pull16(c) + 1; break;
    case 0x6B: /* RTL */
        cy = 6;
        c->pc = npull16(c) + 1;
        c->pbr = npull8(c);
        nfix(c);
        break;
    case 0x40: /* RTI */
        cy = c->e ? 6 : 7;
        setp(c, pull8(c));
        c->pc = pull16(c);
        if (!c->e)
            c->pbr = pull8(c);
        break;
    case 0x00: fetch8(c); cy = c->e ? 7 : 8; interrupt(c, 0xFFE6, 0xFFFE); break;   /* BRK */
    case 0x02: fetch8(c); cy = c->e ? 7 : 8; interrupt(c, 0xFFE4, 0xFFF4); break;   /* COP */

    /* ---- branches ---- */
    case 0x10: cy = branch(c, !(c->p & EMU_N)); break;
    case 0x30: cy = branch(c, c->p & EMU_N); break;
    case 0x50: cy = branch(c, !(c->p & EMU_V)); break;
    case 0x70: cy = branch(c, c->p & EMU_V); break;
    case 0x90: cy = branch(c, !(c->p & EMU_C)); break;
    case 0xB0: cy = branch(c, c->p & EMU_C); break;
    case 0xD0: cy = branch(c, !(c->p & EMU_Z)); break;
    case 0xF0: cy = branch(c, c->p & EMU_Z); break;
    case 0x80: cy = branch(c, 1); break;
    case 0x82: { /* BRL */
        uint16_t off = fetch16(c);
        cy = 4;
        c->pc += off;
        break;
    }

    /* ---- block moves: one byte a step, the PC held until A runs out ---- */
    case 0x54: case 0x44: {
        uint8_t dst = fetch8(c), src = fetch8(c);
        c->dbr = dst;
        wr(c, ((uint32_t) dst << 16) | c->y, rd(c, ((uint32_t) src << 16) | c->x));
        if (op == 0x54) {
            c->x++;
            c->y++;
        } else {
            c->x--;
            c->y--;
        }
        if (X8(c)) {
            c->x &= 0xFF;
            c->y &= 0xFF;
        }
        c->a--;
        if (c->a != 0xFFFF)
            c->pc -= 3;
        cy = 7;
        break;
    }

    /* ---- the rest ---- */
    case 0xEA: break;                                   /* NOP */
    case 0x42: { uint8_t n = fetch8(c); if (c->wdm) c->wdm(c, n); break; }   /* WDM */
    case 0xDB: cy = 4; c->stopped = 1; break;           /* STP */
    case 0xCB: cy = 4; c->stopped = 1; break;           /* WAI: no interrupts here */
    }
    c->cycles += cy;
    return cy;
}
