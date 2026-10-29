/* GCC-style asm() (tccasmraw.c asm816_*): r, a, x, y, i, m and matching
   operands, + (read and written), %H (a pointer's high word), %[name],
   %%, %= labels, the x and y clobbers keeping a loop's index and counter
   out of X and Y, no-operand asm (% copied as it is), __asmraw__, asm at
   file scope (a routine called from C), and 816-opt leaving a block as
   written (two reads of the test register, $3F00, both kept) */
#include "test.h"

#define TICK (*(volatile u16 *) 0x003F00)

u16 g, gm, graw, graw2, tab[8];
u8 bytes[4];
volatile u16 va = 300, vb = 45, vx = 0x1234, vi = 5;

/* a routine in assembly: u16 asm_double(u16 x), the argument on the stack
   above the 3-byte return address, the result in tcc__r0 */
asm("asm_double:\n"
    "    lda 4,s\n"
    "    asl a\n"
    "    sta.b tcc__r0\n"
    "    rtl");
u16 asm_double(u16 x);

static u16 add_r(u16 a, u16 b)
{
    u16 s;
    asm("lda.b %1\n clc\n adc.b %2\n sta.b %0" : "=r"(s) : "r"(a), "r"(b));
    return s;
}

static u16 swap_a(u16 x)
{
    u16 v;
    asm volatile("xba" : "=a"(v) : "a"(x));
    return v;
}

static u16 table_x(u16 i)
{
    u16 v;
    asm("lda.l %1,x" : "=a"(v) : "m"(tab[0]), "x"(i * 2));
    return v;
}

static u16 named(u16 in)
{
    u16 out;
    asm("lda.b %[src]\n eor.w #$FFFF\n sta.b %[dst]" : [dst] "=r"(out) : [src] "r"(in));
    return out;
}

/* a loop's counter is in Y, its index in X: an asm clobbering them (raw
   bytes, so only the clobber list says so) must keep them in memory */
static u16 clobber_y(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        asm(".db $A0, $00, $00" ::: "y");   /* ldy.w #0 */
        t += i;
    }
    return t;
}

static u16 clobber_x(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        t += tab[i];
        asm(".db $A2, $00, $00" ::: "x");   /* ldx.w #0 */
    }
    return t;
}

static u16 loop_asm(u16 times)
{
    u16 r;
    asm("ldx.b %1\n lda.w #0\nlp%=:\n clc\n adc.w #3\n dex\n bne lp%=\n sta.b %0" : "=r"(r) : "r"(times) : "x");
    asm("ldx.w #2\nlp%=:\n inc a\n dex\n bne lp%=" ::: "x", "a");   /* a second lp%=: another number */
    return r;
}

int main(void)
{
    u16 c = 41, d, e = 6, k, lo, bank, base, x, i, t = 0;
    u16 *p = &tab[2];

    for (i = 0; i < 8; i++)
        tab[i] = i * 100;

    CHECK(add_r(va, vb), 345);
    CHECK(swap_a(vx), 0x3412);
    CHECK(table_x(vi), 500);
    CHECK(named(0x00FF), 0xFF00);

    asm("inc.b %0" : "+r"(c));                          /* + : read and written */
    CHECK(c, 42);
    asm("asl.b %0" : "=r"(d) : "0"(e));                 /* the input in output 0's register */
    CHECK(d, 12);
    asm("lda.w #%1\n sta.b %0" : "=r"(k) : "i"(1234));  /* a constant */
    CHECK(k, 1234);
    asm("lda.w #%1\n sta.b %0" : "=r"(lo) : "i"(&g));   /* an address's low word */
    CHECK(lo, (u16) (u32) &g);
    asm("lda.b %H1\n sta.b %0" : "=r"(bank) : "r"(p));  /* a pointer's high word */
    CHECK(bank, (u16) ((u32) p >> 16));
    asm("lda.w #77\n sta.l %0" : "=m"(gm));             /* memory, written by the asm */
    CHECK(gm, 77);
    asm("lda.w #%%1010\n sta.b %0" : "=r"(x));          /* %% */
    CHECK(x, 10);

    asm("lda.w #%0000000000000110\n sta.l graw");      /* no operands: % as it is */
    CHECK(graw, 6);
    __asmraw__("lda.w #5\n sta.l graw2");
    CHECK(graw2, 5);

    CHECK(clobber_y(vi), 0 + 1 + 2 + 3 + 4);
    CHECK(clobber_x(vi), 0 + 100 + 200 + 300 + 400);
    CHECK(loop_asm(vi), 15);
    CHECK(asm_double(21), 42);

    /* the optimiser keeps both reads (and the dead-looking first one) */
    base = TICK;
    asm("lda.l $003F00\n lda.l $003F00\n sta.b %0" : "=r"(x));
    CHECK(x, base + 2);
    CHECK(TICK, base + 3);

    /* a local busy enough for a direct page slot, as r: copied in */
    for (i = 0; i < 8; i++)
        t += i;
    for (i = 0; i < 8; i++)
        t += i;
    asm("lda.b %1\n sta.b %0" : "=r"(x) : "r"(t));
    CHECK(x, 56);
    return 0;
}
