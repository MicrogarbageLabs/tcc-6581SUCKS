/* stack parameters in direct page slots (816-gen.c gfunc_prolog: ranked
   by the dry run like locals, copied in from the stack once the slots are
   saved): a pointer parameter walked directly, a count parameter, a byte,
   one whose address is taken (stays on the stack), one changed (the
   caller's copy isn't), recursion, a call in between (the slots are the
   callee's to save), more parameters than slots, with a fastcall slot */
#include "test.h"

u16 w[8];
u8 b8[8];
volatile u16 vn = 4;

static u16 walk(const u16 *src, u16 n)
{
    u16 t = 0;
    while (n--)
        t += *src++;
    return t;
}

static u16 walk8(const u8 *src, u8 n)
{
    u16 t = 0;
    while (n--)
        t += *src++;
    return t;
}

static void bump(u16 *p) { *p += 100; }
static u16 addr_taken(u16 x, u16 n)
{
    u16 i;
    for (i = 0; i < n; i++)
        x += i;
    bump(&x);                   /* x stays on the stack */
    return x;
}

static u16 changed(u16 x)
{
    u16 i;
    for (i = 0; i < 4; i++)
        x = x * 2;
    return x;
}

static u16 depth(u16 n, const u16 *p)
{
    u16 i, t = 0;
    for (i = 0; i < 2; i++)
        t += p[i];
    return n ? t + depth(n - 1, p + 1) : t;
}

static u16 id(u16 v) { u16 k, r = 0; for (k = 0; k < 2; k++) r += v; return r / 2; }
static u16 across_call(u16 a, const u16 *p)
{
    u16 i, t = 0;
    for (i = 0; i < 3; i++)
        t += id(p[i]) + a;      /* id's slots are its own */
    return t;
}

static u16 many(u16 a, u16 b, u16 c, u16 d, u16 e, const u16 *p)
{
    u16 i, t = 0;
    for (i = 0; i < 2; i++)
        t += a + b + c + d + e + *p++;
    return t;
}

static u16 __attribute__((fastcall)) fast_walk(u16 n, const u16 *src)
{
    u16 t = 0;
    while (n--)
        t += *src++;            /* n in a fastcall slot, src in a dp slot */
    return t;
}

int main(void)
{
    u16 i, x = 5;

    for (i = 0; i < 8; i++) {
        w[i] = i + 1;
        b8[i] = (u8) (i * 40);
    }
    CHECK(walk(w, vn), 1 + 2 + 3 + 4);
    CHECK(walk(w + 4, 4), 5 + 6 + 7 + 8);
    CHECK(walk8(b8, 7), 0 + 40 + 80 + 120 + 160 + 200 + 240);
    CHECK(addr_taken(10, vn), 10 + 6 + 100);
    CHECK(changed(x), 80);
    CHECK(x, 5);
    CHECK(depth(2, w), (1 + 2) + (2 + 3) + (3 + 4));
    CHECK(across_call(10, w), 1 + 2 + 3 + 30);
    CHECK(many(1, 2, 3, 4, 5, w), 2 * 15 + 1 + 2);
    CHECK(fast_walk(vn, w), 10);
    return 0;
}
