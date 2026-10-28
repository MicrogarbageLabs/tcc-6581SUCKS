/* fastcall / regparm(n): the first 16-bit parameters in the direct page
   slots tcc__a0..a3. Leaf functions, functions that call (their slots are
   reused and put back), recursion, arguments swapped between slots,
   parameters written then used after a call, chars, slot and stack
   parameters mixed, regparm(n), and calls through a function pointer */
#include "test.h"

#define FAST __attribute__((fastcall))

volatile u16 va = 1234, vb = 56, vc = 7, vd = 1000;
volatile u32 vl = 0x00012345ULL;
u16 trace;

static u16 FAST add3(u16 a, u16 b, u16 c) { return a + b + c; }
static u16 FAST sub2(u16 a, u16 b) { return a - b; }
static u16 FAST four(u16 a, u16 b, u16 c, u16 d) { return a * 1000 + b * 100 + c * 10 + d; }
static u16 FAST five(u16 a, u16 b, u16 c, u16 d, u16 e) { return four(a, b, c, d) * 10 + e; }

/* calls inside: a and b must survive them */
static u16 FAST outer(u16 a, u16 b)
{
    u16 x = add3(b, a, 1);      /* the slots reused, swapped */
    u16 y = sub2(a, b);
    return x * 100 + y + a + b;
}

/* swapped straight through: g(b, a) */
static u16 FAST swap(u16 a, u16 b) { return sub2(b, a); }

/* recursion through the slots */
static u16 FAST fib(u16 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* a parameter written, then a call, then read */
static u16 FAST bump(u16 a, u16 b)
{
    a += 5;
    trace = add3(a, b, 0);
    a += 1;
    return a + b;
}

/* chars and a mixed list: u32 can't be in a slot, so it goes on the stack */
static u16 FAST chars(u8 a, s8 b, u16 c) { return a + b + c; }
static u32 FAST mixed(u16 a, u32 big, u16 b) { return big + a + b; }

/* regparm(2): two slots, the rest on the stack */
static u16 __attribute__((regparm(2))) rp(u16 a, u16 b, u16 c, u16 d) { return a * 1000 + b * 100 + c * 10 + d; }

/* a plain function calling fastcall ones, and the other way round */
static u16 plain(u16 a, u16 b) { return add3(a, b, 1) + sub2(a, b); }
static u16 FAST via_plain(u16 a, u16 b) { return plain(b, a) + a; }

typedef u16 (FAST *fp2)(u16, u16);

int main(void)
{
    fp2 f = sub2;

    CHECK(add3(va, vb, vc), 1297);
    CHECK(sub2(va, vb), 1178);
    CHECK(four(1, 2, 3, 4), 1234);
    CHECK(five(1, 2, 3, 4, 5), 12345);
    CHECK(outer(10, 3), (3 + 10 + 1) * 100 + 7 + 13);
    CHECK(swap(5, 20), 15);
    CHECK(fib(12), 144);
    CHECK(bump(1, 2), 1 + 5 + 1 + 2);
    CHECK(trace, 8);
    CHECK(chars(200, -3, 1000), 1197);
    CHECK(mixed(1, vl, 2), 0x00012348ULL);
    CHECK(rp(1, 2, 3, 4), 1234);
    CHECK(plain(10, 3), 14 + 7);
    CHECK(via_plain(10, 3), (3 + 10 + 1) + (3 - 10) + 10);
    CHECK(f(vd, va), (u16) (1000 - 1234));
    CHECK(add3(sub2(va, vb), add3(1, 2, 3), four(0, 0, 0, vc)), 1178 + 6 + 7);
    return 0;
}
