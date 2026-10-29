/* static inline functions expanded where they're called (tccgen.c
   inline_expand): a return value, arguments named like the parameters
   (and swapped), each argument evaluated once, conversion to the
   parameter's type, several returns, a void function with an early
   return, a loop and locals in the body, one inline calling another, in
   expressions and conditions, pointers and bytes; and the ones that stay
   ordinary calls: recursion, goto, a static local (shared by every call),
   the address taken */
#include "test.h"

u16 arr[8], g, hits;
volatile u16 v5 = 5, v3 = 3;

static inline u16 sq(u16 x) { return x * x; }
static inline u16 sub(u16 y, u16 x) { return y - x; }
static inline u8 lo(u8 v) { return v; }
static inline s16 neg(s16 v) { return -v; }
static inline u16 *at(u16 i) { return &arr[i]; }
static inline u16 clamp(u16 v, u16 hi)
{
    if (v > hi)
        return hi;
    if (v == 0)
        return 1;
    return v;
}
static inline void store(u16 v)
{
    if (v == 0)
        return;                 /* early return */
    g += v;
}
static inline u16 sum_to(u16 n)
{
    u16 i, t = 0;
    for (i = 1; i <= n; i++)
        t += i;
    return t;
}
static inline u16 sq_plus(u16 a) { return sq(a) + 1; }     /* inline in inline */
static inline u16 is_zero(u16 v) { return v == 0; }

/* these stay calls */
static inline u16 fact(u16 n) { return n < 2 ? 1 : n * fact(n - 1); }
static inline u16 first_odd(u16 n)
{
    u16 i;
    for (i = 0; i < n; i++)
        if (arr[i] & 1)
            goto found;
    return 99;
found:
    return i;
}
static inline u16 count(void)
{
    static u16 c;
    return ++c;
}

static u16 loop_sq(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += sq(i);             /* no call left: the loop can keep i in Y */
    return t;
}

int main(void)
{
    u16 x = 10, y = 3, i, r;
    u16 (*fp)(u16) = sq;

    for (i = 0; i < 8; i++)
        arr[i] = i * 2;

    CHECK(sq(v5), 25);
    CHECK(sub(x, y), 7);        /* sub's y is the caller's x */
    CHECK(sub(y, x), (u16) -7);
    i = 2;
    r = sq(i++);                /* the argument once */
    CHECK(r, 4);
    CHECK(i, 3);
    CHECK(lo(0x1234), 0x34);    /* converted to the parameter's type */
    CHECK(neg(v5), -5);        /* an s16, widened */
    *at(3) = 99;
    CHECK(arr[3], 99);
    arr[3] = 6;
    CHECK(clamp(50, 20), 20);
    CHECK(clamp(0, 20), 1);
    CHECK(clamp(v5, 20), 5);
    g = 0;
    store(4);
    store(0);
    store(v3);
    CHECK(g, 7);
    CHECK(sum_to(v5), 15);
    CHECK(sq_plus(v3), 10);
    CHECK(sq(v3) + sq(v5), 34);
    CHECK(sub(sq(v5), sq(v3)), 16);
    r = 0;
    if (is_zero(v5 - 5))
        r = 1;
    CHECK(r, 1);
    CHECK(fact(v5), 120);
    arr[5] = 11;
    CHECK(first_odd(8), 5);
    CHECK(count(), 1);
    CHECK(count(), 2);
    CHECK(fp(7), 49);
    CHECK(loop_sq(v5), 0 + 1 + 4 + 9 + 16);
    return 0;
}
