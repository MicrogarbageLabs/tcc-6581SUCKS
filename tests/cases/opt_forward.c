/* operand forwarding in 816-opt (lda M / sta rN / ... / adc rN -> adc M,
   sta rN / ... / ldx rN -> tax): the cases where forwarding would be
   wrong, so it must not happen, next to the cases where it does */
#include "test.h"

u16 g, h, arr[8];
u8 b8[8];
volatile u16 vk = 3;

static u16 sub(u16 a, u16 b) { return a - b; }

/* the memory changes between the park and the use */
static u16 alias_store(u16 x)
{
    u16 t;
    t = g;
    g = 5;
    return t + x;
}

/* ... through a pointer */
static u16 alias_ptr(u16 *p, u16 x)
{
    u16 t;
    t = g;
    *p = 9;
    return x - t;
}

/* ... by an increment of the memory itself */
static u16 alias_inc(u16 x)
{
    u16 t;
    t = h;
    h++;
    return t & x;
}

/* ... by a call */
static u16 bump(void) { g += 100; return 1; }
static u16 alias_call(u16 x)
{
    u16 t;
    t = g;
    return t + bump() + x;
}

/* a local, the stack moving between (a call pushes its arguments) */
static u16 stack_local(u16 a, u16 b)
{
    u16 t = a;
    return sub(b, 1) + t;
}

/* indexes: the parked value is an index, X is used in between */
static u16 idx_between(u16 i, u16 j)
{
    return arr[i] + arr[j] + b8[i];
}

/* compares, subtractions and logic forwarded */
static u16 cmp_mem(u16 x) { return (x < g) + (x == h) * 2 + (x > g) * 4; }
static u16 logic_mem(u16 x) { return (x & g) ^ (x | h); }

int main(void)
{
    u16 i;

    g = 10;
    CHECK(alias_store(1), 11);
    CHECK(g, 5);

    g = 30;
    CHECK(alias_ptr(&g, 100), 70);
    CHECK(g, 9);

    h = 0x00F0;
    CHECK(alias_inc(0x0FFF), 0x00F0);
    CHECK(h, 0x00F1);

    g = 7;
    CHECK(alias_call(1), 7 + 1 + 1);
    CHECK(g, 107);

    CHECK(stack_local(20, 50), 69);

    for (i = 0; i < 8; i++) {
        arr[i] = i * 11;
        b8[i] = (u8) (i + 200);
    }
    CHECK(idx_between(vk, vk + 2), 33 + 55 + 203);

    g = 50;
    h = 40;
    CHECK(cmp_mem(40), 1 + 2);
    CHECK(cmp_mem(60), 4);
    CHECK(cmp_mem(50), 0);
    CHECK(logic_mem(0x0F0F), (0x0F0F & 50) ^ (0x0F0F | 40));
    return 0;
}
