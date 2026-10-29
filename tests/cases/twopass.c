/* each function compiled twice (a dry run that measures, then the real
   one): what the dry run must leave as it found it, and what it measures.
   goto labels and switch cases (rewound), static locals and strings (not
   allocated twice), a shadowed name (two locals, measured apart), an
   address taken behind parentheses (no slot), recursion, and many locals
   competing for four slots */
#include "test.h"

u16 *kept;
volatile u16 vn = 6;

static u16 counter(void)
{
    static u16 calls = 0;       /* once: the dry run must not allocate it */
    calls++;
    return calls;
}

static const char *greeting(void) { return "dry"; }

static u16 jumps(u16 n)
{
    u16 i = 0, t = 0;
again:
    if (i < n) {
        t += i;
        i++;
        goto again;
    }
    switch (t % 4) {
    case 0: t += 100; break;
    case 1: t += 200; break;
    case 3: t += 300; break;
    default: t += 400; break;
    }
    return t;
}

static u16 shadow(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        u16 i = 7;              /* another i */
        t += i;
    }
    for (i = 0; i < n; i++)
        t += i;
    return t + i;
}

static u16 addr_taken(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += i;
    kept = &(t);                /* t lives on the stack */
    *kept += 1000;
    return t;
}

static u16 fact(u16 n) { return n < 2 ? 1 : n * fact(n - 1); }

/* a call's result widened inside an expression: tcc's no-code mode (an
   earlier dry run used it) left the 32-bit value half built here */
static u8 bytes[4] = { 0x78, 0x56, 0x34, 0x12 };
static u16 rd16(u16 at) { return (u16) bytes[at] | ((u16) bytes[at + 1] << 8); }
static u32 rd32(u16 at) { return (u32) rd16(at) | ((u32) rd16(at + 2) << 16); }

static u16 many(u16 n)
{
    u16 a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, i;
    for (i = 0; i < n; i++) {
        a += 1; b += 2; c += 3;
        d += a; e += b; f += c;
    }
    return a + b + c + d + e + f;
}

int main(void)
{
    const char *g;

    CHECK(counter(), 1);
    CHECK(counter(), 2);
    g = greeting();
    CHECK(g[0], 'd');
    CHECK(g[2], 'y');
    CHECK(jumps(vn), 15 + 300);      /* 15 % 4 == 3 */
    CHECK(shadow(vn), 42 + 15 + 6);
    CHECK(addr_taken(vn), 15 + 1000);
    CHECK(fact(vn), 720);
    CHECK(rd32(0), 0x12345678ULL);
    CHECK(many(vn), 6 + 12 + 18 + 21 + 42 + 63);
    return 0;
}
