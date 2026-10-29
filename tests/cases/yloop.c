/* a for loop's counter in Y (816-gen.c yl_*, chosen by the dry run): the
   ways out (the test, break, return), continue, the counter changed or
   read in the body, after the loop and between two loops, nested loops
   (the inner one gets Y), and the loops that must not get it (a call, a
   pointer read through [rN],y, a goto, the address taken, a byte counter,
   a global counter) */
#include "test.h"

u16 arr[32], g;
u8 b8[32];
volatile u16 vn = 10;

static u16 after_test(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += i;
    return t * 100 + i;         /* i back from Y at the exit */
}

static u16 after_break(u16 n)
{
    u16 i;
    for (i = 0; i < n; i++)
        if (arr[i] == 7)
            break;
    return i;                   /* ... and at a break */
}

static u16 from_inside(u16 n)
{
    u16 i;
    for (i = 0; i < n; i++)
        if (arr[i] == 5)
            return i + 1000;    /* a return: nothing to write back */
    return 0;
}

static u16 with_continue(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        if (i & 1)
            continue;
        t += i;
    }
    return t + i;
}

static u16 changed_in_body(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        t += i;
        if (i == 3)
            i += 2;             /* written inside: ldy */
    }
    return t * 100 + i;
}

static u16 two_loops(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += i;
    for (i = i; i < n + 3; i++)
        t += 100;
    return t + i;
}

static u16 down(u16 n)
{
    u16 i, t = 0;
    for (i = n; i > 0; i--)
        arr[i] = i;
    for (i = n; i != 0; i -= 2)
        t += arr[i];
    return t;
}

static u16 nested(u16 n)
{
    u16 i, j, t = 0;
    for (i = 0; i < n; i++)
        for (j = 0; j < i; j++)
            t += j;
    return t + i * 1000 + j;
}

static u16 step3(u16 n)
{
    u16 i, t = 0;
    for (i = 1; i < n; i += 3)
        t += arr[i] + b8[i];
    return t;
}

/* the ones that must keep the counter in memory */
static u16 id(u16 x) { return x; }
static u16 has_call(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += id(i);
    return t;
}

static u16 has_ptr(u16 *p, u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += p[i] + p[1];
    return t;
}

static u16 has_goto(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        if (i == 4)
            goto out;
        t += i;
    }
out:
    return t * 100 + i;
}

static void bump(u16 *p) { *p += 1; }
static u16 addr_taken(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        t += i;
        if (i == 2)
            bump(&i);
    }
    return t * 100 + i;
}

static u16 byte_counter(u16 n)
{
    u8 i;
    u16 t = 0;
    for (i = 250; i != (u8) (250 + n); i++)
        t += i;
    return t;
}

static u16 global_counter(u16 n)
{
    u16 t = 0;
    for (g = 0; g < n; g++)
        t += g;
    return t + g;
}

static u16 forever(u16 n)
{
    u16 i;
    for (i = 0;; i++)
        if (i * i > n)
            break;
    return i;
}

int main(void)
{
    u16 i, ref;

    for (i = 0; i < 32; i++) {
        arr[i] = i;
        b8[i] = (u8) (i * 2);
    }

    CHECK(after_test(vn), 4500 + 10);
    CHECK(after_break(vn), 7);
    CHECK(after_break(5), 5);
    CHECK(from_inside(vn), 1005);
    CHECK(from_inside(3), 0);
    CHECK(with_continue(vn), 20 + 10);
    CHECK(changed_in_body(vn), (0 + 1 + 2 + 3 + 6 + 7 + 8 + 9) * 100 + 10);
    CHECK(two_loops(vn), 45 + 300 + 13);
    CHECK(down(vn), 10 + 8 + 6 + 4 + 2);
    for (i = 0; i < 32; i++)
        arr[i] = i;
    ref = 0;
    for (i = 0; i < 6; i++) {
        u16 j;
        for (j = 0; j < i; j++)
            ref += j;
    }
    CHECK(nested(6), ref + 6000 + 5);
    CHECK(step3(vn), (1 + 4 + 7) * 3);
    CHECK(has_call(vn), 45);
    CHECK(has_ptr(arr, vn), 45 + 10);
    CHECK(has_goto(vn), 6 * 100 + 4);
    CHECK(addr_taken(vn), (0 + 1 + 2 + 4 + 5 + 6 + 7 + 8 + 9) * 100 + 10);
    CHECK(byte_counter(4), 250 + 251 + 252 + 253);
    CHECK(byte_counter(8), 250 + 251 + 252 + 253 + 254 + 255 + 0 + 1);
    CHECK(global_counter(vn), 45 + 10);
    CHECK(forever(vn), 4);
    return 0;
}
